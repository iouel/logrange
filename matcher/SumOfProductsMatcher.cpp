// SumOfProductsMatcher.cpp — LLVM opt plugin. RECOGNITION ONLY, no rewriting
// (intent Deliverable 2 precondition: measure the hit rate before building
// any transform). Match criteria are fixed in METHODOLOGY.md; this file
// implements them and nothing more.
//
// Emits greppable lines on stderr, one per event:
//   LOOP,<file>,<line>,<function>                       innermost FP loop examined
//   HIT,<file>,<line>,<function>,<trip>,<depth>,<nmul>,<transcendental|plain>,<risk>,<reasons>
//   XLOOP,<file>,<line>,<function>,<risk>               only under
//                                                       sop-matcher<xloop>
//   WARN,not-simplified,<file>,<line>,<function>        pipeline is missing
//                                                       loop-simplify; nothing
//                                                       can match. Never gated.
// risk is the profitability signal — the gate in front of any rewrite:
// "would this reduction actually underflow?" HIGH | MED | LOW, with
// semicolon-joined reason tokens (or "none"). Aggregation happens in the
// run script, not here.
//
// Usage: opt -load-pass-plugin=./SopMatcher.so \
//            -passes=loop-simplify,lcssa,sop-matcher -disable-output module.bc
//
// The two canonicalization passes are REQUIRED: they are unstated
// preconditions of RecurrenceDescriptor::AddReductionVar, which this file
// delegates recognition to. Omit them and every loop is skipped with a WARN.

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace llvm;

namespace {

// Names whose presence in the product chain marks likelihood-style math —
// the exact shape the project targets. Matched by substring on the callee.
bool isTranscendentalName(StringRef Name) {
  return Name.contains("exp") || Name.contains("log") ||
         Name.contains("pow") || Name.contains("sqrt");
}

// Risk-relevant sub-families of the above. Exp-family (incl. pow): a factor
// whose magnitude the exponent controls — unbounded, the underflow smoking
// gun. Log-family: inputs already in log domain being multiplied back.
bool isExpFamilyName(StringRef Name) {
  return Name.contains("exp") || Name.contains("pow");
}
bool isLogFamilyName(StringRef Name) { return Name.contains("log"); }

// Known libm calls accepted in the chain even though they may write errno
// (the errno store makes them non-readonly at IR level; it is irrelevant to
// the reduction's shape). Exact names only — substring would admit anything.
bool isKnownLibmCall(StringRef N) {
  return N == "exp" || N == "expf" || N == "exp2" || N == "exp2f" ||
         N == "expm1" || N == "expm1f" || N == "log" || N == "logf" ||
         N == "log2" || N == "log2f" || N == "log10" || N == "log10f" ||
         N == "log1p" || N == "log1pf" || N == "pow" || N == "powf" ||
         N == "sqrt" || N == "sqrtf" || N == "fabs" || N == "fabsf" ||
         N == "sin" || N == "sinf" || N == "cos" || N == "cosf" ||
         N == "tanh" || N == "tanhf" || N == "erf" || N == "erfc";
}

struct ChainInfo {
  unsigned depth = 0;          // instructions visited in the term chain
  unsigned nMul = 0;           // fmul / fmuladd count
  bool transcendental = false; // readonly exp/log/pow/sqrt calls in chain
  bool expChain = false;       // exp-family (exp/expm1/exp2/pow) call in chain
  bool logChain = false;       // log-family call in chain
  bool ok = true;              // chain stayed within the allowed op set
  // Underlying objects the term chain LOADS FROM. Collected only so the
  // cross-loop rule can ask whether this reduction consumes what an earlier
  // outer iteration produced; nothing in the risk grading reads it. The walk
  // treated Load as a pure leaf until 2026-08-21 and discarded the address,
  // which was the one fact that rule needs.
  SmallPtrSet<const Value *, 8> loadObjects;
};

// Walk the definition chain of the reduction term (the non-accumulator
// operand of the update), staying inside the loop. Allowed interior ops per
// METHODOLOGY.md: fmul, fadd, fsub, fneg, fp casts, int->fp casts, loads,
// selects, and calls to read-only FP functions. Anything else (stores,
// side-effecting calls, another phi — a nested reduction) fails the chain.
void walkChain(Value *V, const Loop &L, ChainInfo &CI,
               SmallPtrSetImpl<Value *> &Visited, unsigned Budget = 64) {
  // A rejection is FINAL — never reset ok on entry. (Earlier version wrote
  // "CI.ok = CI.depth <= Budget" here, silently un-rejecting a dirty chain
  // whenever a sibling operand was visited next; caught when a supposedly
  // more permissive matcher produced fewer hits.)
  if (!CI.ok) return;
  if (CI.depth > Budget) { CI.ok = false; return; }
  auto *I = dyn_cast<Instruction>(V);
  if (!I || !L.contains(I)) return; // loop-invariant / constant / argument: leaf
  if (!Visited.insert(V).second) return;
  ++CI.depth;

  switch (I->getOpcode()) {
  case Instruction::FMul:
  case Instruction::FDiv: // a/b is a product with a reciprocal factor —
                          // exactly what log_div handles; in scope
    ++CI.nMul;
    [[fallthrough]];
  case Instruction::FAdd:
  case Instruction::FSub:
  case Instruction::FNeg:
  case Instruction::FPExt:
  case Instruction::FPTrunc:
  case Instruction::SIToFP:
  case Instruction::UIToFP:
  case Instruction::Select:
    for (Use &U : I->operands()) walkChain(U.get(), L, CI, Visited, Budget);
    return;
  case Instruction::Load:
    // Still a leaf for SHAPE purposes — the address computation is not FP
    // shape and is deliberately not walked. The underlying object is
    // recorded on the way past.
    CI.loadObjects.insert(
        getUnderlyingObject(cast<LoadInst>(I)->getPointerOperand()));
    return;
  case Instruction::PHI:
    // A DIFFERENT loop-carried value feeding the term (e.g. a recurrence
    // variable) is just an input from the term's point of view: leaf. It
    // cannot be the accumulator phi — that has exactly one in-loop user,
    // its spine consumer, checked at the call site. (Audit finding: cheb
    // series error accumulators were missed without this.)
    return;
  case Instruction::Call: {
    auto *CB = cast<CallBase>(I);
    if (auto *II = dyn_cast<IntrinsicInst>(CB)) {
      if (II->getIntrinsicID() == Intrinsic::fmuladd ||
          II->getIntrinsicID() == Intrinsic::fma) {
        ++CI.nMul;
        for (Use &U : II->args()) walkChain(U.get(), L, CI, Visited, Budget);
        return;
      }
    }
    if (!CB->isIndirectCall()) {
      Function *Callee = CB->getCalledFunction();
      StringRef Name = Callee ? Callee->getName() : StringRef();
      if (CB->onlyReadsMemory() || isKnownLibmCall(Name)) {
        if (isTranscendentalName(Name)) CI.transcendental = true;
        if (isExpFamilyName(Name)) CI.expChain = true;
        if (isLogFamilyName(Name)) CI.logChain = true;
        for (Use &U : CB->args()) walkChain(U.get(), L, CI, Visited, Budget);
        return;
      }
    }
    CI.ok = false;
    return;
  }
  default:
    CI.ok = false;
    return;
  }
}

// ---------------------------------------------------------------------------
// Cross-loop feedback detection.
//
// Risk is graded one loop at a time, which is why the forward algorithm ends
// up LOW: each inner reduction looks unremarkable while the magnitude decays
// across the ENCLOSING loop. This asks the one structural question that
// distinguishes that family from a benign nested dot product: does this
// reduction consume, on a later outer iteration, the object it produces?
//
//   for (t)                      <- parent loop P
//     for (j)
//       for (i) s += buf[i]*A[..];   <- innermost reduction, terms load buf
//       out[j] = s;                  <- result stored ... to buf, next round
//
// The proxy is underlying-object identity between the reduction's store and
// one of its term loads. Full alias/dependence analysis stays out of scope,
// exactly as it did when the mid-loop-read guard refinement was declined.
//
// WHAT THIS DOES NOT ESTABLISH: that the magnitude actually decays. It
// establishes feedback, which is the structural precondition for decay. A
// power iteration that renormalises every step feeds back and does not decay.
// The token is named for what is detected, not for what is feared.
// Resolve an underlying object to the SET of buffers it can actually be.
//
// The textbook forward algorithm alternates two buffers:
//     double *prev = buf0, *cur = buf1;
//     for (t) { ...read prev, write cur...; swap(prev, cur); }
// so getUnderlyingObject on the store gives the `cur` phi and on the load the
// `prev` phi — two different values, and a plain identity test sees no
// feedback at all. Resolving each phi through its incoming values gives
// {buf0, buf1} for both, and the intersection is what the rule is really
// asking about. `forward_full_swap` in coverage.c exists to keep an
// identity-only rule from looking finished.
void resolveObjects(const Value *V, SmallPtrSetImpl<const Value *> &Out,
                    SmallPtrSetImpl<const Value *> &Seen, unsigned Depth = 6) {
  if (!V || !Seen.insert(V).second || Depth == 0) return;
  const Value *U = getUnderlyingObject(const_cast<Value *>(V));
  if (const auto *P = dyn_cast<PHINode>(U)) {
    for (const Value *In : P->incoming_values())
      resolveObjects(In, Out, Seen, Depth - 1);
    return;
  }
  if (const auto *Sel = dyn_cast<SelectInst>(U)) {
    resolveObjects(Sel->getTrueValue(), Out, Seen, Depth - 1);
    resolveObjects(Sel->getFalseValue(), Out, Seen, Depth - 1);
    return;
  }
  Out.insert(U);
}

bool objectsOverlap(const Value *A,
                    const SmallPtrSetImpl<const Value *> &LoadObjects) {
  SmallPtrSet<const Value *, 8> AObjs, Seen;
  resolveObjects(A, AObjs, Seen);
  for (const Value *L : LoadObjects) {
    SmallPtrSet<const Value *, 8> LObjs, LSeen;
    resolveObjects(L, LObjs, LSeen);
    for (const Value *O : LObjs)
      if (AObjs.count(O)) return true;
  }
  return false;
}

bool storesIntoOwnInput(const Loop &L, const Instruction *Upd,
                        const SmallPtrSetImpl<const Value *> &LoadObjects) {
  const Loop *P = L.getParentLoop();
  if (!P || LoadObjects.empty()) return false;

  // Candidate stores of the reduction's result: anything reachable from the
  // update that is a store, inside the parent but outside this loop (the
  // register form, stored after the inner loop) or inside it (the
  // memory-carried form, where RecurrenceDescriptor already accepted the
  // store as part of the reduction).
  SmallVector<const Value *, 8> Work{Upd};
  SmallPtrSet<const Value *, 16> Seen{Upd};
  while (!Work.empty()) {
    const Value *V = Work.pop_back_val();
    for (const User *U : V->users()) {
      const auto *UI = dyn_cast<Instruction>(U);
      if (!UI || !P->contains(UI)) continue;
      if (const auto *St = dyn_cast<StoreInst>(UI)) {
        // Only a store OF the value counts; a store of something else that
        // merely happens to use it as an address operand does not.
        if (St->getValueOperand() != V) continue;
        if (objectsOverlap(St->getPointerOperand(), LoadObjects))
          return true;
        continue;
      }
      // Follow LCSSA phis and value-preserving-ish FP ops out of the loop:
      // `out[j] = s * B[j]` is still the reduction's result reaching memory.
      if (!isa<PHINode>(UI) && !UI->getType()->isFloatingPointTy()) continue;
      if (Seen.insert(UI).second) Work.push_back(UI);
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Reduction recognition.
//
// "Is this loop a floating-point reduction" is a solved compiler problem, and
// this file is deliberately NOT its second authority: RecurrenceDescriptor —
// the analysis the loop vectorizer trusts — answers it. What stays here is
// walkChain and the risk grading below it, because no compiler answers "would
// this reduction underflow".
//
// This replaced a hand-written spine walk. The swap was measured against it
// over the whole study corpus before the local copy was deleted: +32 hits,
// -1, no change to any risk grade or to the five HIGH findings. See
// matcher/DELTA.md, and matcher/data/raw-delta.txt for the paired records.
// ---------------------------------------------------------------------------
struct Recognition {
  bool matched = false;
  SmallVector<Value *, 8> Terms; // non-spine operands feeding the accumulator
  unsigned SpineMuls = 0;        // fmuladd/fma count on the spine itself
  const char *why = "";          // when !matched: the check that turned it away
};

// Recover the reduction cycle: in-loop instructions lying on a path from Phi
// to the backedge value. Used only when getReductionOpChain declines to order
// the chain (it answers a vectorizer question, not a recognition one, and
// isReductionPHI has already said yes by the time we get here).
//
// This is bookkeeping, not recognition: LLVM has no API returning "the terms
// of a reduction" because no LLVM client needs them. walkChain does.
bool collectCycle(PHINode &Phi, const Loop &L, Value *Back,
                  SmallVectorImpl<Instruction *> &Out) {
  SmallPtrSet<Instruction *, 16> Fwd, Bwd;
  SmallVector<Instruction *, 16> Stack;

  // Forward from the phi through in-loop users.
  for (User *U : Phi.users())
    if (auto *UI = dyn_cast<Instruction>(U))
      if (L.contains(UI) && Fwd.insert(UI).second) Stack.push_back(UI);
  while (!Stack.empty()) {
    Instruction *I = Stack.pop_back_val();
    for (User *U : I->users())
      if (auto *UI = dyn_cast<Instruction>(U))
        if (L.contains(UI) && !isa<PHINode>(UI) && Fwd.insert(UI).second)
          Stack.push_back(UI);
  }

  // Backward from the backedge value through in-loop operands.
  if (auto *BI = dyn_cast<Instruction>(Back))
    if (L.contains(BI) && Bwd.insert(BI).second) Stack.push_back(BI);
  while (!Stack.empty()) {
    Instruction *I = Stack.pop_back_val();
    for (Use &U : I->operands())
      if (auto *OI = dyn_cast<Instruction>(U.get()))
        if (L.contains(OI) && !isa<PHINode>(OI) && Bwd.insert(OI).second)
          Stack.push_back(OI);
  }

  // The cycle is the intersection, in deterministic (program) order.
  for (BasicBlock *BB : L.blocks())
    for (Instruction &I : *BB)
      if (Fwd.count(&I) && Bwd.count(&I)) Out.push_back(&I);
  return !Out.empty();
}

Recognition recognizeLlvm(PHINode &Phi, Loop *L, Value *Back, DominatorTree &DT,
                          AssumptionCache &AC, DemandedBits &DB,
                          ScalarEvolution &SE) {
  Recognition R;

  // Unstated precondition, found by segfault: AddReductionVar obtains the
  // start value with Phi->getIncomingValueForBlock(L->getLoopPreheader())
  // and never null-checks the preheader. With assertions off that is an
  // out-of-bounds read, not a diagnostic. Loop-simplify form is required;
  // the legacy walk needed no such thing, since it entered from the latch.
  // Recover these by running loop-simplify ahead of the pass.
  if (!L->isLoopSimplifyForm()) {
    R.why = "not-simplified";
    return R;
  }

  RecurrenceDescriptor RD;
  if (!RecurrenceDescriptor::isReductionPHI(&Phi, L, RD, &DB, &AC, &DT, &SE)) {
    R.why = "not-reduction";
    return R;
  }

  // Only additive FP reductions are in scope. FMul (total *= x) is a product,
  // not a sum of products, and is rescuable by exponent tracking without
  // logsumexp; FMin/FMax/AnyOf are not reductions this project rescues at all.
  // METHODOLOGY.md, "what counts as a hit", criterion 2.
  const RecurKind K = RD.getRecurrenceKind();
  if (K != RecurKind::FAdd && K != RecurKind::FMulAdd) {
    R.why = "kind";
    return R;
  }

  // getReductionOpChain answers a vectorizer question — can these operations
  // be treated as in-loop reduction steps — which is stricter than what the
  // term walk needs. It declined 63 times on the study corpus where
  // isReductionPHI had already said yes, so the fallback is load-bearing.
  SmallVector<Instruction *, 4> Chain = RD.getReductionOpChain(&Phi, L);
  if (Chain.empty() && !collectCycle(Phi, *L, Back, Chain)) {
    R.why = "chain";
    return R;
  }

  // Terms are the operands of cycle instructions that are not themselves on
  // the cycle: exactly what feeds the accumulator each iteration. fmuladd on
  // the spine contributes a multiply the term walk cannot see (its operands
  // are terms, not an fmul instruction), so count it here. At -O1 clang folds
  // `acc += a*b` into llvm.fmuladd(a, b, acc), so missing this would drop
  // nMul to 0 on the most common shape in the corpus.
  SmallPtrSet<Instruction *, 8> OnCycle(Chain.begin(), Chain.end());
  SmallPtrSet<Value *, 16> Seen;
  for (Instruction *I : Chain) {
    if (RecurrenceDescriptor::isFMulAddIntrinsic(I) ||
        (isa<IntrinsicInst>(I) &&
         cast<IntrinsicInst>(I)->getIntrinsicID() == Intrinsic::fma))
      ++R.SpineMuls;
    for (Use &U : I->operands()) {
      Value *V = U.get();
      if (V == &Phi) continue;
      if (auto *VI = dyn_cast<Instruction>(V))
        if (OnCycle.count(VI)) continue;
      if (!V->getType()->isFloatingPointTy()) continue; // metadata, fn operands
      if (Seen.insert(V).second) R.Terms.push_back(V);
    }
  }
  if (R.Terms.empty()) {
    R.why = "no-terms";
    return R;
  }

  R.matched = true;
  return R;
}

// Everything downstream of recognition: the term walk, the sum-of-products
// gate, and the profitability triage. This is the part no compiler provides,
// and it is identical for both recognizers — which is what makes a DIFF record
// meaningful. A divergence is always a recognition divergence.
struct Verdict {
  bool hit = false;
  bool crossLoop = false;      // reduction feeds its own input across a parent
  const char *why = "";
  ChainInfo CI;
  const char *Trip = "unknown";
  const char *Risk = "LOW";
  std::string Reasons;
};

Verdict evaluate(const Recognition &R, Loop *L, ScalarEvolution &SE,
                 const Instruction *Upd) {
  Verdict V;
  if (!R.matched) {
    V.why = R.why;
    return V;
  }

  V.CI.nMul = R.SpineMuls;
  SmallPtrSet<Value *, 32> Visited;
  for (Value *T : R.Terms) walkChain(T, *L, V.CI, Visited);
  if (!V.CI.ok) { // dirty chain: miss
    V.why = "dirtyChain";
    return V;
  }
  // nMul was standing in for "this term's magnitude can compound". An
  // exp-family factor does that on its own: exp(t) spans the whole range from
  // t alone, no multiply needed. Requiring a multiply excluded the softmax
  // denominator — this project's marquee shape, and the one the rewrite pass
  // implements. darknet's softmax matched only because it divides by a
  // temperature (FDiv counts as a multiply); the textbook
  // `sum += exp(x[i] - max)` was invisible. Plain sums with no exponent stay
  // out of scope: they are rescuable without logsumexp, which is why
  // plain_sum is still a labeled miss.
  if (V.CI.nMul == 0 && !V.CI.expChain) {
    V.why = "noMulNoExp";
    return V;
  }

  if (SE.getSmallConstantTripCount(L) > 0) V.Trip = "constant";
  else if (SE.hasLoopInvariantBackedgeTakenCount(L)) V.Trip = "runtime";

  // Profitability triage (the gate in front of any rewrite): would this
  // reduction actually underflow? HIGH iff an exp-family factor is in the
  // chain (unbounded magnitude); MED for many multiplied factors, or
  // log-domain inputs multiplied together; else LOW.
  const bool deepChain = V.CI.nMul >= 4;
  const bool unknownTrip = StringRef(V.Trip) == "unknown";
  V.Risk = V.CI.expChain ? "HIGH"
           : (deepChain || (V.CI.logChain && V.CI.nMul >= 2)) ? "MED"
                                                              : "LOW";
  // Detected but NOT graded. The tier this should map to changes the
  // published HIGH count, so the signal is measured on the corpus first and
  // the mapping decided from real numbers; the detection rule itself is fixed
  // here, in advance, which is the half METHODOLOGY.md's ordering is about.
  V.crossLoop = storesIntoOwnInput(*L, Upd, V.CI.loadObjects);

  SmallVector<const char *, 5> Reasons;
  if (V.CI.expChain) Reasons.push_back("exp-chain");
  // Separately tagged so the pre-2026-08-15 counts stay recoverable: every hit
  // carrying exp-sum is one the nMul >= 1 rule used to drop.
  if (V.CI.nMul == 0) Reasons.push_back("exp-sum");
  if (V.CI.logChain) Reasons.push_back("log-chain");
  if (deepChain) Reasons.push_back("deep-chain");
  if (unknownTrip) Reasons.push_back("unknown-trip");
  // crossLoop is deliberately NOT a reason token. It is measured, not
  // graded: see the XLOOP census below and matcher/XLOOP.md for why
  // promoting on it was declined on the evidence. Putting it here would
  // rewrite data/raw-*.txt and would imply the grading acts on it.
  if (Reasons.empty()) {
    V.Reasons = "none";
  } else {
    for (size_t i = 0; i < Reasons.size(); ++i) {
      if (i) V.Reasons += ";";
      V.Reasons += Reasons[i];
    }
  }

  V.hit = true;
  return V;
}

// The HIT payload, i.e. everything after the source location. Two verdicts are
// the same verdict iff these strings match.
std::string payload(const Verdict &V) {
  if (!V.hit) return std::string("miss:") + V.why;
  return (Twine(V.Trip) + "," + Twine(V.CI.depth) + "," + Twine(V.CI.nMul) +
          "," + (V.CI.transcendental ? "transcendental" : "plain") + "," +
          V.Risk + "," + V.Reasons)
      .str();
}

void printLoc(raw_ostream &OS, const Instruction *I, const Function &F) {
  if (const DebugLoc &DL = I->getDebugLoc()) {
    OS << DL->getFilename() << "," << DL.getLine();
  } else {
    OS << "<nodbg>,0";
  }
  OS << "," << F.getName();
}

// ---------------------------------------------------------------------------
// THE LOG-IFIABLE PREDICATE (matcher/RESCUE.md, R1).
//
// The rescue study needs each term's LOG-MAGNITUDE, taken symbolically from
// the chain. Capturing the term's VALUE cannot see the case the diagnostic
// exists for: in `s += w[i]*exp(logp[i])` at logp ~ -800 the term is already
// 0.0 when the accumulator sees it, so linear, reference and log-reference all
// read zero and the marquee site scores as no-failure.
//
// This walk decides, statically and mechanically, whether a chain can be
// decomposed that way, and emits a postfix descriptor for the ones that can.
// matcher/rescue_shim.cpp replays it. The two must agree on the op vocabulary;
// the shim's header carries the same table with the arithmetic.
//
// FOUR RULES ARE NARROWER THAN THE OBVIOUS ONES, each because the obvious one
// would corrupt the ground truth:
//
//   - fptrunc is NOT a pass-through. A healthy double can be 0.0f after
//     narrowing, so the destination rounding is modelled at replay. Emitted as
//     TRUNCF rather than skipped.
//   - pow gets a sign only from the observed base and an integrality test on
//     the observed exponent, which is a runtime decision. Statically it is
//     just POW.
//   - An in-loop CALL is not a leaf unless it is one of the decomposable
//     functions below. Taking log|value| of an arbitrary call result
//     reconstructs whatever the callee already collapsed internally.
//   - fadd/fsub are decomposable, but their arithmetic must keep signed
//     cancellation in double-double, which is the shim's job.
//
// A loop-INVARIANT value is a leaf, including an invariant call: it is an
// input to the reduction under study rather than part of it. An in-loop call
// is part of the computation being measured and has to be decomposed or
// declined.
// ---------------------------------------------------------------------------

// Functions whose log-magnitude decomposes exactly. Everything else in-loop is
// UNLOGIFIABLE. expm1 is deliberately absent: exp(a)-1 has no clean log form.
const char *decomposableOp(StringRef Name) {
  if (Name == "exp" || Name == "expf" || Name == "llvm.exp.f64" ||
      Name == "llvm.exp.f32")
    return "EXP";
  if (Name == "exp2" || Name == "exp2f" || Name == "llvm.exp2.f64" ||
      Name == "llvm.exp2.f32")
    return "EXP2";
  if (Name == "pow" || Name == "powf" || Name == "llvm.pow.f64" ||
      Name == "llvm.pow.f32")
    return "POW";
  if (Name == "sqrt" || Name == "sqrtf" || Name == "llvm.sqrt.f64" ||
      Name == "llvm.sqrt.f32")
    return "SQRT";
  if (Name == "fabs" || Name == "fabsf" || Name == "llvm.fabs.f64" ||
      Name == "llvm.fabs.f32")
    return "ABS";
  return nullptr;
}

struct ChainDesc {
  std::string postfix;
  SmallVector<Value *, 16> leaves;
  bool ok = true;
  const char *why = "";

  void op(const char *o) {
    if (!postfix.empty()) postfix += " ";
    postfix += o;
  }
  void leaf(Value *V) {
    const unsigned slot = leaves.size();
    leaves.push_back(V);
    if (!postfix.empty()) postfix += " ";
    postfix += "L" + std::to_string(slot);
  }
  void fail(const char *reason) {
    if (ok) { ok = false; why = reason; }
  }
};

void buildDesc(Value *V, const Loop &L, ChainDesc &D, unsigned depth = 0) {
  if (!D.ok) return;
  if (depth > 32) { D.fail("chain-too-deep"); return; }
  if (D.leaves.size() > 32) { D.fail("too-many-leaves"); return; }

  auto *I = dyn_cast<Instruction>(V);
  if (!I || !L.contains(I)) { D.leaf(V); return; } // invariant: an input

  switch (I->getOpcode()) {
  case Instruction::FMul:
    buildDesc(I->getOperand(0), L, D, depth + 1);
    buildDesc(I->getOperand(1), L, D, depth + 1);
    D.op("MUL");
    return;
  case Instruction::FDiv:
    buildDesc(I->getOperand(0), L, D, depth + 1);
    buildDesc(I->getOperand(1), L, D, depth + 1);
    D.op("DIV");
    return;
  case Instruction::FAdd:
    buildDesc(I->getOperand(0), L, D, depth + 1);
    buildDesc(I->getOperand(1), L, D, depth + 1);
    D.op("ADD");
    return;
  case Instruction::FSub:
    buildDesc(I->getOperand(0), L, D, depth + 1);
    buildDesc(I->getOperand(1), L, D, depth + 1);
    D.op("SUB");
    return;
  case Instruction::FNeg:
    buildDesc(I->getOperand(0), L, D, depth + 1);
    D.op("NEG");
    return;
  case Instruction::FPExt:
    buildDesc(I->getOperand(0), L, D, depth + 1);
    D.op("EXT");
    return;
  case Instruction::FPTrunc:
    // Not transparent. The narrowing can take a healthy double to 0.0f, and
    // the replay applies that rounding.
    if (!I->getType()->isFloatTy()) { D.fail("fptrunc-not-f32"); return; }
    buildDesc(I->getOperand(0), L, D, depth + 1);
    D.op("TRUNCF");
    return;
  case Instruction::Load:
  case Instruction::PHI:
    D.leaf(I); // program data, and a phi is an input from the term's view
    return;
  case Instruction::Call: {
    auto *CB = cast<CallBase>(I);
    if (CB->isIndirectCall()) { D.fail("indirect-call"); return; }
    Function *Callee = CB->getCalledFunction();
    const char *op = Callee ? decomposableOp(Callee->getName()) : nullptr;
    if (!op) { D.fail("opaque-call"); return; }
    for (unsigned i = 0; i < CB->arg_size(); ++i)
      buildDesc(CB->getArgOperand(i), L, D, depth + 1);
    D.op(op);
    return;
  }
  default:
    D.fail("unsupported-op");
    return;
  }
}

// ---------------------------------------------------------------------------
// SopInstrumentPass — emits the recording calls rescue_shim.cpp consumes.
//
// A SEPARATE pass from SopMatcherPass on purpose. That one is recognition
// only and returns PreservedAnalyses::all(); RESCUE.md requires the HIT stream
// to be byte-identical with the instrument on and off, and the cleanest way to
// guarantee that is for the HIT-emitting pass never to gain a transform.
// Recognition itself is shared, not duplicated: same file, same
// recognizeLlvm, same walkChain.
// ---------------------------------------------------------------------------
struct SopInstrumentPass : PassInfoMixin<SopInstrumentPass> {
  static int nextId;

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &AC = AM.getResult<AssumptionAnalysis>(F);
    auto &DB = AM.getResult<DemandedBitsAnalysis>(F);

    Module *M = F.getParent();
    LLVMContext &C = F.getContext();
    Type *Dbl = Type::getDoubleTy(C);
    Type *I32 = Type::getInt32Ty(C);
    PointerType *I8P = PointerType::getUnqual(C);

    FunctionCallee FSite = M->getOrInsertFunction(
        "lr_site", FunctionType::get(Type::getVoidTy(C),
                                     {I32, I8P, I8P, I32, I32, Dbl}, false));
    FunctionCallee FLeaf = M->getOrInsertFunction(
        "lr_leaf",
        FunctionType::get(Type::getVoidTy(C), {I32, I32, Dbl}, false));
    FunctionCallee FTerm = M->getOrInsertFunction(
        "lr_term", FunctionType::get(Type::getVoidTy(C), {I32, Dbl}, false));
    FunctionCallee FExec = M->getOrInsertFunction(
        "lr_exec", FunctionType::get(Type::getVoidTy(C), {I32, Dbl}, false));

    bool changed = false;

    for (Loop *L : LI.getLoopsInPreorder()) {
      if (!L->isInnermost()) continue;
      BasicBlock *Latch = L->getLoopLatch();
      BasicBlock *Pre = L->getLoopPreheader();
      if (!Latch || !Pre) continue;

      for (PHINode &Phi : L->getHeader()->phis()) {
        if (!Phi.getType()->isFloatingPointTy()) continue;
        Value *Back = Phi.getIncomingValueForBlock(Latch);
        auto *Upd = dyn_cast<Instruction>(Back);
        if (!Upd || !L->contains(Upd)) continue;

        const Verdict V = evaluate(recognizeLlvm(Phi, L, Back, DT, AC, DB, SE),
                                   L, SE, Upd);
        if (!V.hit) continue;

        auto decline = [&](const char *reason) {
          errs() << "UNLOGIFIABLE,";
          printLoc(errs(), Upd, F);
          errs() << "," << reason << "," << V.Risk << "\n";
        };

        // The term is the update's non-accumulator contribution. Two spine
        // shapes carry it, and BOTH are required rather than optional:
        // which one clang emits is -ffp-contract, a flag this study does not
        // control. Measured on this control file at the study's own flags:
        // the default contracts `s += a*b` into llvm.fmuladd, so a
        // fadd-only instrument declines the marquee shape everywhere.
        // pass/ELIGIBILITY.md 3.1 records the same fact for the rewrite.
        const unsigned Op = Upd->getOpcode();
        Value *Term = nullptr;      // single-value term, when there is one
        Value *MulA = nullptr, *MulB = nullptr; // fmuladd's two factors
        bool negate = false;

        if (Op == Instruction::FAdd || Op == Instruction::FSub) {
          if (Upd->getOperand(0) == &Phi) {
            Term = Upd->getOperand(1);
            negate = (Op == Instruction::FSub);
          } else if (Upd->getOperand(1) == &Phi) {
            Term = Upd->getOperand(0);
          } else {
            decline("phi-not-a-spine-operand");
            continue;
          }
        } else if (auto *II = dyn_cast<IntrinsicInst>(Upd)) {
          const Intrinsic::ID IID = II->getIntrinsicID();
          if ((IID == Intrinsic::fmuladd || IID == Intrinsic::fma) &&
              II->getArgOperand(2) == &Phi) {
            MulA = II->getArgOperand(0);
            MulB = II->getArgOperand(1);
          } else {
            decline("spine-not-recognized");
            continue;
          }
        } else {
          decline("spine-not-recognized");
          continue;
        }

        Type *TermTy = Term ? Term->getType() : MulA->getType();
        if (!TermTy->isDoubleTy() && !TermTy->isFloatTy()) {
          decline("term-type-unsupported");
          continue;
        }

        BasicBlock *Exit = L->getUniqueExitBlock();
        if (!Exit) { decline("no-unique-exit"); continue; }

        ChainDesc D;
        if (Term) {
          buildDesc(Term, *L, D);
        } else {
          buildDesc(MulA, *L, D);
          buildDesc(MulB, *L, D);
          D.op("MUL");
        }
        if (negate) D.op("NEG");
        if (!D.ok) { decline(D.why); continue; }
        if (D.leaves.empty()) { decline("no-leaves"); continue; }

        const int id = nextId++;
        std::string loc;
        {
          raw_string_ostream OS(loc);
          printLoc(OS, Upd, F);
        }

        IRBuilder<> B(Pre->getTerminator());
        Value *LocS = B.CreateGlobalString(loc);
        Value *ChainS = B.CreateGlobalString(D.postfix);
        B.CreateCall(FSite,
                     {ConstantInt::get(I32, id), LocS, ChainS,
                      ConstantInt::get(I32, (int)D.leaves.size()),
                      ConstantInt::get(I32, Phi.getType()->isFloatTy() ? 32 : 64),
                      ConstantFP::get(Dbl, 1e-10)});

        // Leaves and the term are recorded immediately before the update, so
        // every recorded value dominates the call by construction.
        B.SetInsertPoint(Upd);
        auto widen = [&](Value *X) -> Value * {
          return X->getType()->isDoubleTy() ? X : B.CreateFPExt(X, Dbl);
        };
        for (unsigned i = 0; i < D.leaves.size(); ++i)
          B.CreateCall(FLeaf, {ConstantInt::get(I32, id),
                               ConstantInt::get(I32, (int)i),
                               widen(D.leaves[i])});
        // For a contracted spine the term is not a value in the IR, so the
        // product is formed here purely for the record. The original
        // fmuladd is untouched.
        Value *TermVal = Term ? Term : B.CreateFMul(MulA, MulB);
        B.CreateCall(FTerm, {ConstantInt::get(I32, id), widen(TermVal)});

        // The reduction's live-out value, in the exit block.
        Value *ExitVal = Upd;
        for (PHINode &EP : Exit->phis())
          for (unsigned i = 0; i < EP.getNumIncomingValues(); ++i)
            if (EP.getIncomingValue(i) == Upd) { ExitVal = &EP; break; }
        B.SetInsertPoint(Exit->getFirstNonPHIIt());
        B.CreateCall(FExec, {ConstantInt::get(I32, id), widen(ExitVal)});

        errs() << "INSTRUMENT,";
        printLoc(errs(), Upd, F);
        errs() << "," << V.Risk << "," << D.leaves.size() << "," << D.postfix
               << "\n";
        changed = true;
      }
    }
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

int SopInstrumentPass::nextId = 0;

struct SopMatcherPass : PassInfoMixin<SopMatcherPass> {
  // Explain mode: emit a REJECT record naming the check that turned a
  // candidate away, and for the mid-loop-read guard, what the extra in-loop
  // user was. Off by default and byte-silent when off — the study's raw
  // output must not change shape because a diagnostic exists.
  //
  // This exists because RESULTS.md publishes a table of rejection causes.
  // That table was first produced by a throwaway instrumented build living
  // on one machine, which made it unreproducible; the numbers are only
  // evidence if a reader can regenerate them. Drive it with
  //   -passes='loop-simplify,lcssa,sop-matcher<explain>'
  // or via ./run_study.sh rejects <name>.
  bool Explain = false;
  // Weight census: for hits with the mixture spine w * exp(t), classify the
  // multiplicand that is NOT the exp. The rewrite pass can only fold a
  // weight into the reference when its magnitude is provably safe, so this
  // answers whether implementing that is worth doing at all.
  bool Weights = false;
  // Cross-loop feedback census. One XLOOP record per hit whose result is
  // stored into an object its own terms load from, across the enclosing
  // loop. Off by default and byte-silent when off, exactly like Weights:
  // the study's raw output must not change shape because a census exists.
  bool XLoop = false;
  explicit SopMatcherPass(bool Explain = false, bool Weights = false,
                          bool XLoop = false)
      : Explain(Explain), Weights(Weights), XLoop(XLoop) {}

  void reject(const char *Why, const Instruction *Upd,
              const Function &F) const {
    if (!Explain) return;
    errs() << "REJECT," << Why << ",";
    printLoc(errs(), Upd, F);
    errs() << "\n";
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    // Required by RecurrenceDescriptor::isReductionPHI. DB/AC/DT drive the
    // minimal-bit-width computation (irrelevant for FP, but the API takes
    // them); SE is what lets it process stores to loop-invariant addresses.
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &AC = AM.getResult<AssumptionAnalysis>(F);
    auto &DB = AM.getResult<DemandedBitsAnalysis>(F);

    for (Loop *L : LI.getLoopsInPreorder()) {
      if (!L->isInnermost()) continue;

      // Only loops with FP arithmetic are "examined" (denominator of the
      // reported hit rate — integer loops are out of scope entirely).
      bool hasFP = false;
      for (BasicBlock *BB : L->blocks())
        for (Instruction &I : *BB)
          if (I.getType()->isFloatingPointTy() &&
              (isa<BinaryOperator>(I) || isa<UnaryOperator>(I) ||
               isa<CallBase>(I))) {
            hasFP = true;
            break;
          }
      if (!hasFP) continue;

      // Anchor the LOOP line on the first debug-located instruction so the
      // line number is usable (header phis typically carry no debug loc).
      Instruction *Anchor = &*L->getHeader()->begin();
      for (BasicBlock *BB : L->blocks()) {
        for (Instruction &I : *BB)
          if (I.getDebugLoc()) { Anchor = &I; break; }
        if (Anchor->getDebugLoc()) break;
      }
      errs() << "LOOP,";
      printLoc(errs(), Anchor, F);
      errs() << "\n";

      BasicBlock *Latch = L->getLoopLatch();
      if (!Latch) continue; // irregular loop: cannot be a simple reduction

      // Loop-simplify form is a precondition of the recognizer, and a caller
      // that omits `loop-simplify,lcssa` from the pipeline would otherwise get
      // a clean, empty, entirely wrong report. This record is NOT gated on
      // Explain for that reason: a silent tool reads as a passing one.
      // (Found on 2026-08-17 when pass/run_pass_test.sh, a third caller, kept
      // the bare -passes=sop-matcher form and gate 3d failed with "rewritten
      // but the matcher emits no HIT".)
      if (!L->isLoopSimplifyForm()) {
        errs() << "WARN,not-simplified,";
        printLoc(errs(), Anchor, F);
        errs() << "\n";
        continue;
      }

      // Candidate accumulators: FP phis in the header whose backedge value
      // is an in-loop fadd/fsub with the phi as one operand.
      for (PHINode &Phi : L->getHeader()->phis()) {
        if (!Phi.getType()->isFloatingPointTy()) continue;
        Value *Back = Phi.getIncomingValueForBlock(Latch);
        auto *Upd = dyn_cast<Instruction>(Back);
        if (!Upd || !L->contains(Upd)) continue;

        const Verdict V = evaluate(recognizeLlvm(Phi, L, Back, DT, AC, DB, SE),
                                   L, SE, Upd);
        const ChainInfo &CI = V.CI;
        if (!V.hit) {
          reject(V.why, Upd, F);
          continue;
        }

        errs() << "HIT,";
        printLoc(errs(), Upd, F);
        errs() << "," << payload(V) << "\n";

        if (XLoop && V.crossLoop) {
          errs() << "XLOOP,";
          printLoc(errs(), Upd, F);
          errs() << "," << V.Risk << "\n";
        }

        // Weight census. For the mixture spine w * exp(t), the rewrite pass
        // must fold the weight's magnitude into the running reference or the
        // scaled sum can overflow where the linear original does not. It can
        // only do that when the weight is provably safe, so this records what
        // the weights in real code actually ARE. One record per multiply on
        // whose operands exactly one side reaches an exp.
        if (Weights && CI.expChain) {
          auto reachesExp = [&](Value *V) {
            ChainInfo Sub;
            SmallPtrSet<Value *, 32> Seen;
            walkChain(V, *L, Sub, Seen);
            return Sub.expChain;
          };
          auto classify = [](Value *W) -> const char * {
            if (auto *C = dyn_cast<ConstantFP>(W))
              return C->getValueAPF().isNegative() ? "const-negative"
                                                   : "const-nonneg";
            if (auto *CB2 = dyn_cast<CallBase>(W))
              if (Function *CF = CB2->getCalledFunction()) {
                StringRef N = CF->getName();
                if (N.starts_with("llvm.fabs")) return "fabs";
                if (N.starts_with("llvm.sqrt")) return "sqrt";
                if (N.starts_with("llvm.exp")) return "exp";
                return "call";
              }
            if (auto *BO = dyn_cast<BinaryOperator>(W))
              if (BO->getOpcode() == Instruction::FMul &&
                  BO->getOperand(0) == BO->getOperand(1))
                return "square";
            if (isa<LoadInst>(W)) return "load";
            if (isa<PHINode>(W)) return "phi";
            if (isa<UIToFPInst>(W)) return "uitofp";
            if (isa<Argument>(W)) return "argument";
            return "other";
          };
          auto emit = [&](Value *A, Value *B) {
            const bool ea = reachesExp(A), eb = reachesExp(B);
            if (ea == eb) return; // both or neither: not a w * exp(t) pair
            errs() << "WEIGHT," << classify(ea ? B : A) << ",";
            printLoc(errs(), Upd, F);
            errs() << "\n";
          };
          for (BasicBlock *BB : L->blocks())
            for (Instruction &I2 : *BB) {
              if (auto *BO = dyn_cast<BinaryOperator>(&I2)) {
                if (BO->getOpcode() == Instruction::FMul)
                  emit(BO->getOperand(0), BO->getOperand(1));
              } else if (auto *II2 = dyn_cast<IntrinsicInst>(&I2)) {
                if (II2->getIntrinsicID() == Intrinsic::fmuladd)
                  emit(II2->getArgOperand(0), II2->getArgOperand(1));
              }
            }
        }
      }
    }
    return PreservedAnalyses::all();
  }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "SopMatcher", "0.1", [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "sop-matcher") {
                    FPM.addPass(SopMatcherPass(/*Explain=*/false));
                    return true;
                  }
                  // Parameters go inside <> and are semicolon-separated: the
                  // new-PM pipeline parser splits on commas at the top level,
                  // so a comma form never reaches this callback intact.
                  if (Name == "sop-matcher<weights>") {
                    FPM.addPass(SopMatcherPass(/*Explain=*/false,
                                               /*Weights=*/true));
                    return true;
                  }
                  if (Name == "sop-matcher<xloop>") {
                    FPM.addPass(SopMatcherPass(/*Explain=*/false,
                                               /*Weights=*/false,
                                               /*XLoop=*/true));
                    return true;
                  }
                  if (Name == "sop-matcher<explain>") {
                    FPM.addPass(SopMatcherPass(/*Explain=*/true));
                    return true;
                  }
                  // The rescue instrument (matcher/RESCUE.md, R1). A separate
                  // pass, not a mode: this one TRANSFORMS, and the HIT stream
                  // must stay byte-identical whether or not it runs.
                  if (Name == "sop-instrument") {
                    FPM.addPass(SopInstrumentPass());
                    return true;
                  }
                  return false;
                });
          }};
}
