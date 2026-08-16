// SumOfProductsMatcher.cpp — LLVM opt plugin. RECOGNITION ONLY, no rewriting
// (intent Deliverable 2 precondition: measure the hit rate before building
// any transform). Match criteria are fixed in METHODOLOGY.md; this file
// implements them and nothing more.
//
// Emits greppable lines on stderr, one per event:
//   LOOP,<file>,<line>,<function>                       innermost FP loop examined
//   HIT,<file>,<line>,<function>,<trip>,<depth>,<nmul>,<transcendental|plain>,<risk>,<reasons>
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
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
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
    return; // memory read: leaf (the address computation is not FP shape)
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
  const char *why = "";
  ChainInfo CI;
  const char *Trip = "unknown";
  const char *Risk = "LOW";
  std::string Reasons;
};

Verdict evaluate(const Recognition &R, Loop *L, ScalarEvolution &SE) {
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
  SmallVector<const char *, 4> Reasons;
  if (V.CI.expChain) Reasons.push_back("exp-chain");
  // Separately tagged so the pre-2026-08-15 counts stay recoverable: every hit
  // carrying exp-sum is one the nMul >= 1 rule used to drop.
  if (V.CI.nMul == 0) Reasons.push_back("exp-sum");
  if (V.CI.logChain) Reasons.push_back("log-chain");
  if (deepChain) Reasons.push_back("deep-chain");
  if (unknownTrip) Reasons.push_back("unknown-trip");
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
  explicit SopMatcherPass(bool Explain = false, bool Weights = false)
      : Explain(Explain), Weights(Weights) {}

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
                                   L, SE);
        const ChainInfo &CI = V.CI;
        if (!V.hit) {
          reject(V.why, Upd, F);
          continue;
        }

        errs() << "HIT,";
        printLoc(errs(), Upd, F);
        errs() << "," << payload(V) << "\n";

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
                  if (Name == "sop-matcher<explain>") {
                    FPM.addPass(SopMatcherPass(/*Explain=*/true));
                    return true;
                  }
                  return false;
                });
          }};
}
