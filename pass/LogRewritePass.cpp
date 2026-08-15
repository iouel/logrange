// LogRewritePass.cpp — LLVM opt plugin. LogRange intent step 8: the first
// REWRITE prototype. Deliberately narrow: exactly one shape — the softmax
// denominator idiom (sum += exp(t_i)) — rewritten to streaming logsumexp
// state, with the matcher study's discipline: decline anything not provably
// this shape. Companion recognition-only pass: matcher/SumOfProductsMatcher.cpp.
//
// Shape matched (ALL conditions required, no fallbacks):
//   - innermost loop with preheader, unique latch, unique exiting block,
//     unique exit block
//   - exactly one FP-typed phi in the header, of type double, initialized
//     to constant 0.0 from the preheader
//   - its backedge update is a plain fadd(phi, X)  (fmuladd out of scope)
//   - X is a call to llvm.exp.*, possibly through fpext/fptrunc only
//     (source-level exp/expf declined: errno — see the errno contract below)
//   - the call argument is loop-varying (an instruction inside the loop)
//   - the phi and the update have no other in-loop users (the matcher's
//     mid-loop-read guard: a prefix-sum-style read would change meaning)
//   - at least one out-of-loop user of the sum exists (else nothing
//     observable would change)
//
// Rewrite: streaming logsumexp with rescaling, straight-line — no CFG
// surgery inside the body; llvm.maxnum plays the role of the select:
//   header:  m = phi double [ -inf, preheader ], [ newm,  latch ]
//            s = phi double [  0.0, preheader ], [ snext, latch ]
//   body:    newm  = llvm.maxnum(m, t)              ; t = the exp() argument
//            dm    = (m oeq newm) ? 0.0 : m - newm  ; inf - inf guard
//            dt    = (t oeq newm) ? 0.0 : t - newm
//            snext = s * llvm.exp(dm) + llvm.exp(dt)
//   exit:    logsum = newm + llvm.log(snext)        ; == log(sum) exactly
//            sum'   = llvm.exp(logsum)              ; linear replacement
//            store logsum -> @__logrange_logsum     ; prototype export hook
//
// Derivation: s_new = s*exp(m - newm) + exp(t - newm) with newm = max(m, t).
// Both exponents are <= 0, so neither exp can overflow. t > m gives
// s*exp(m-t) + 1 (the rescale step); t <= m gives s + exp(t-m) (the plain
// accumulate step) — one formula covers both branches of the textbook
// update, which is why no select (and no block split) is needed.
// First iteration: m=-inf, s=0 -> newm=t, s*exp(-inf)=0, snext=exp(0)=1. OK.
// Infinite terms: the raw differences are inf - inf = NaN when t = -inf while
// m is still -inf (a zero term, exp(-inf)=0 — ordinary input), and when
// t = +inf. The oeq-guarded differences make both exact: all-(-inf) gives
// newm=-inf, snext=k, exp(newm + log(k)) = 0 = the linear sum; a later finite
// t rescales that state to 0 through exp(-inf - t) = 0.
// NaN stickiness: maxnum(m, NaN)=m, but exp(NaN - newm)=NaN poisons s, and
// s stays NaN through every later iteration; the final exp(newm + log(NaN))
// is NaN — matching the linear loop's NaN propagation.
//
// Legality opt-in (intent Deliverable 2 precondition): the pass only runs
// when explicitly named in -passes — that is the prototype's opt-in — AND
// additionally requires either the function attribute
// "unsafe-fp-math"="true" or the pass parameter force:
//     -passes='log-rewrite<force>'
// Reassociation permission is the caller's grant; this pass never
// self-authorizes it. New instructions carry NO fast-math flags — the grant
// covers the structural reassociation performed here, not further FP
// relaxation of the emitted code.
//
// What force means, precisely: "the caller explicitly grants the
// reassociation this transform needs". It waives reassociation PROOF, and
// nothing else. It does NOT waive, and cannot override:
//   - the structural match conditions above;
//   - strictfp / llvm.experimental.constrained.* rejection;
//   - non-default denormal-fp-math environment rejection;
//   - the llvm.exp-only errno contract;
//   - special-value correctness (the oeq infinity guard).
// Full contract: pass/ELIGIBILITY.md.
//
// Errno contract. The rewrite deletes N source exp evaluations and emits 2N
// different exponentials plus a log, so errno-visible behaviour changes for
// a conforming math_errno program. Matching ONLY llvm.exp.* is therefore
// the errno contract, not an arbitrary narrowing: clang emits the intrinsic
// exactly when errno is already unobservable. Measured, LLVM 21, on the
// test kernel: at -O1 the source `s += exp(x[i])` emits
// `call double @exp`; adding -fno-math-errno emits `llvm.exp.f64`.
//
// FP environment. Functions the pass cannot model are declined outright,
// before any loop is examined, and force does not reach these:
//   strictfp attribute / any llvm.experimental.constrained.* operation /
//   a denormal-fp-math or denormal-fp-math-f32 mode other than IEEE.
//
// Emits one line per rewrite on stderr:  REWRITE,<file>,<line>,<function>
// Declines are logged too: DECLINE-FPENV,<file>,<line>,<fn>,<reason> and
// DECLINE-ERRNO,<file>,<line>,<fn>,external-exp-call.
//
// Usage: opt-21 -load-pass-plugin=./LogRewrite.so \
//               -passes='log-rewrite<force>' -S in.ll -o out.ll

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/FloatingPointMode.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace {

// Prototype export hook: the log-domain result m + log(s) is stored to this
// external global so a caller can observe the value the linear replacement
// cannot represent (exp(logsum) re-underflows exactly when the rescue
// matters). Real design — propagating the log form to downstream uses like
// the softmax divide — is future work; see PROTOTYPE.md.
constexpr const char *ExportGlobalName = "__logrange_logsum";

// How an exp-shaped call may be used by this pass.
//   Intrinsic   — llvm.exp.*: accepted.
//   ExternalExp — a direct call to the libm names exp/expf: DECLINED, and
//                 worth a diagnostic because it is a near miss.
//   No          — not exp at all: silent decline, indistinguishable from
//                 the many other loops in a module.
enum class ExpKind { No, Intrinsic, ExternalExp };

// Errno contract, not a shape preference. The matcher (recognition only)
// treats libm errno as irrelevant, and correctly so: recognizing a shape
// observes nothing. This pass REPLACES the computation — it deletes N
// source exp evaluations and emits 2N different exponentials plus a log —
// so every errno write and FP-exception flag the source loop performed is
// gone or different. For a conforming program compiled with
// math_errno in force, that is observable behaviour.
//
// llvm.exp.* is exactly the marker that errno is already unobservable:
// clang emits it only under -fno-math-errno / -ffast-math (measured on the
// test kernel, LLVM 21 — see the file header). Restricting to the
// intrinsic is therefore the errno contract itself, not a narrowing chosen
// for convenience. force does not waive it: force grants reassociation,
// which has nothing to say about errno.
//
// EXTENSION POINT (deliberately not taken here). A direct external call to
// exp/expf may be accepted once the IR itself proves the call has no
// observable memory or errno effect — i.e. the call site's memory effects
// exclude writes to errno memory and to inaccessible memory
// (CallBase::getMemoryEffects(), checking the ErrnoMem and
// InaccessibleMem locations, which LLVM 21 models explicitly: an
// errno-writing exp declaration carries memory(errnomem: write)).
// Implementing it requires its own decline/accept tests; until those
// exist, external calls are declined unconditionally.
ExpKind classifyExpCall(CallBase *CB, Value *&ArgOut) {
  Value *A = nullptr;
  ExpKind Kind;
  if (auto *II = dyn_cast<IntrinsicInst>(CB)) {
    if (II->getIntrinsicID() != Intrinsic::exp)
      return ExpKind::No;
    A = II->getArgOperand(0);
    Kind = ExpKind::Intrinsic;
  } else {
    if (CB->isIndirectCall())
      return ExpKind::No;
    Function *Callee = CB->getCalledFunction();
    if (!Callee)
      return ExpKind::No;
    StringRef N = Callee->getName();
    if (N != "exp" && N != "expf")
      return ExpKind::No;
    if (CB->arg_size() != 1)
      return ExpKind::No;
    A = CB->getArgOperand(0);
    Kind = ExpKind::ExternalExp;
  }
  if (!A->getType()->isFloatTy() && !A->getType()->isDoubleTy())
    return ExpKind::No;
  ArgOut = A;
  return Kind;
}

// FP-environment screen, applied to the whole function before any loop is
// looked at. Each condition names an environment the emitted streaming
// state cannot preserve; none of them is overridable by force.
//
//   strictfp        — the function opted into a dynamic rounding mode and
//                     strict exception semantics. The rewrite changes both
//                     the number and the operands of the FP operations, so
//                     the exception record and the rounding-mode-dependent
//                     result both change.
//   constrained-fp  — llvm.experimental.constrained.* carries per-operation
//                     rounding/exception metadata that the plain fadd/fmul/
//                     fsub/intrinsics emitted here cannot express.
//   denormal-fp-math — a non-IEEE denormal mode (flush-to-zero,
//                     preserve-sign) changes which intermediate values are
//                     zero. The rewrite's intermediates are exp() of
//                     differences against a running max — a different set
//                     of values entirely from the source loop's exp(x_i) —
//                     so which of them flush is not preservable.
//
// Returns a stable reason token, or nullptr if the environment is the
// ordinary default one.
const char *fpEnvRejectReason(const Function &F) {
  if (F.hasFnAttribute(Attribute::StrictFP))
    return "strictfp";
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB)
      if (isa<ConstrainedFPIntrinsic>(&I))
        return "constrained-fp";
  // getDenormalMode resolves denormal-fp-math for double and
  // denormal-fp-math-f32 for float, defaulting to IEEE when absent.
  if (F.getDenormalMode(APFloat::IEEEdouble()) != DenormalMode::getIEEE() ||
      F.getDenormalMode(APFloat::IEEEsingle()) != DenormalMode::getIEEE())
    return "denormal-fp-math";
  return nullptr;
}

// Sole in-loop user, exactly as in SumOfProductsMatcher.cpp.
const User *soleInLoopUser(const Value *V, const Loop &L) {
  const User *Found = nullptr;
  for (const User *U : V->users()) {
    auto *UI = dyn_cast<Instruction>(U);
    if (UI && L.contains(UI)) {
      if (Found)
        return nullptr; // more than one
      Found = U;
    }
  }
  return Found;
}

void printLoc(raw_ostream &OS, const Instruction *I, const Function &F) {
  if (const DebugLoc &DL = I->getDebugLoc()) {
    OS << DL->getFilename() << "," << DL.getLine();
  } else {
    OS << "<nodbg>,0";
  }
  OS << "," << F.getName();
}

// Function-level declines have no single instruction to blame; use the
// subprogram's own location so the line format stays uniform.
void printFnLoc(raw_ostream &OS, const Function &F) {
  if (const DISubprogram *SP = F.getSubprogram()) {
    OS << SP->getFilename() << "," << SP->getLine();
  } else {
    OS << "<nodbg>,0";
  }
  OS << "," << F.getName();
}

struct ConsumerUse {
  Instruction *User;
  Value *SeenAs;
  bool IsFinalSum;
};

void collectConsumerUses(Value *V, const Loop &L, bool IsFinalSum,
                         SmallVectorImpl<ConsumerUse> &Out,
                         SmallPtrSetImpl<Value *> &SeenVals,
                         SmallPtrSetImpl<Instruction *> &SeenUsers) {
  if (!SeenVals.insert(V).second)
    return;
  for (User *U : V->users()) {
    auto *UI = dyn_cast<Instruction>(U);
    if (!UI || L.contains(UI))
      continue;
    if (isa<PHINode>(UI) ||
        (isa<CastInst>(UI) &&
         (UI->getOpcode() == Instruction::FPExt ||
          UI->getOpcode() == Instruction::FPTrunc))) {
      collectConsumerUses(UI, L, IsFinalSum, Out, SeenVals, SeenUsers);
      continue;
    }
    if (SeenUsers.insert(UI).second)
      Out.push_back({UI, V, IsFinalSum});
  }
}

const char *classifyConsumerUse(const ConsumerUse &CU, const Loop &L) {
  auto *BO = dyn_cast<BinaryOperator>(CU.User);
  if (!BO)
    return CU.IsFinalSum ? "not-fdiv" : "not-the-sum";
  if (BO->getOpcode() != Instruction::FDiv)
    return CU.IsFinalSum ? "not-fdiv" : "not-the-sum";
  if (!CU.IsFinalSum || BO->getOperand(1) != CU.SeenAs)
    return "not-the-sum";
  if (!BO->getType()->isDoubleTy() || !BO->getOperand(0)->getType()->isDoubleTy() ||
      !CU.SeenAs->getType()->isDoubleTy())
    return "not-double";
  if (auto *NumeratorI = dyn_cast<Instruction>(BO->getOperand(0)))
    if (L.contains(NumeratorI))
      return "numeral-ineligible";
  if (!BO->hasOneUse())
    return "shared-result";
  return "fdiv-of-sum";
}

// Risk tiers, ordered, matching SumOfProductsMatcher.cpp's triage. Kept as a
// plain enum rather than shared code because the two plugins build
// independently; the rule they encode is one sentence and is stated in both.
// None is above High deliberately: it is a threshold no verdict can reach, so
// min-risk=none means "decline everything" and gives the gate's refusal path
// something to do while every shape this prototype matches verdicts HIGH.
enum class Risk { Low = 0, Med = 1, High = 2, None = 3 };

const char *riskName(Risk R) {
  switch (R) {
  case Risk::None: return "NONE";
  case Risk::High: return "HIGH";
  case Risk::Med:  return "MED";
  default:         return "LOW";
  }
}

struct LogRewritePass : PassInfoMixin<LogRewritePass> {
  bool Force;
  Risk MinRisk = Risk::High;
  bool PropagateDiv = false;
  explicit LogRewritePass(bool Force, Risk MinRisk = Risk::High,
                          bool PropagateDiv = false)
      : Force(Force), MinRisk(MinRisk), PropagateDiv(PropagateDiv) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    // Opt-in gate. Being named in -passes got us here at all; on top of
    // that, reassociation legality must be granted by the caller: either
    // the function is already marked unsafe-fp-math (e.g. -ffast-math) or
    // the pass was instantiated with the force parameter.
    //
    // "unsafe-fp-math"="true" is retained as an alternate opt-in, and is
    // ONLY that: an explicit user action (-ffast-math /
    // -funsafe-math-optimizations) standing in for the caller's grant of
    // reassociation. It is never read as evidence that special values may
    // be discarded. Nothing below is unlocked by it — NaN/inf/signed-zero
    // correctness, the FP-environment screen and the errno contract all
    // apply identically on both opt-in paths. See pass/ELIGIBILITY.md.
    bool OptIn =
        Force || F.getFnAttribute("unsafe-fp-math").getValueAsString() == "true";
    if (!OptIn)
      return PreservedAnalyses::all();

    auto &LI = AM.getResult<LoopAnalysis>(F);

    // FP-environment screen. Ordered before the loop walk because these are
    // properties of the whole function, and before any use of Force because
    // force may not override them. Reported only for functions that contain
    // a loop: a loop-free function was never a candidate, and logging it
    // would be noise.
    if (const char *Reason = fpEnvRejectReason(F)) {
      if (!LI.empty()) {
        errs() << "DECLINE-FPENV,";
        printFnLoc(errs(), F);
        errs() << "," << Reason << "\n";
      }
      return PreservedAnalyses::all();
    }

    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    bool Changed = false;

    // Snapshot: we add blocks (edge split) but never delete or restructure
    // existing loops, so the preorder walk stays valid.
    SmallVector<Loop *, 8> Loops(LI.getLoopsInPreorder());
    for (Loop *L : Loops) {
      if (!L->isInnermost())
        continue;
      BasicBlock *Header = L->getHeader();
      BasicBlock *Preheader = L->getLoopPreheader();
      BasicBlock *Latch = L->getLoopLatch();
      BasicBlock *ExitingBB = L->getExitingBlock(); // unique or null
      BasicBlock *ExitBB = L->getExitBlock();       // unique or null
      if (!Preheader || !Latch || !ExitingBB || !ExitBB)
        continue;

      // Exactly one FP phi in the header: the accumulator. A second FP phi
      // would be another loop-carried FP value interacting with the sum in
      // ways this prototype does not analyze.
      PHINode *Acc = nullptr;
      bool MultipleFPPhis = false;
      for (PHINode &P : Header->phis()) {
        if (!P.getType()->isFloatingPointTy())
          continue;
        if (Acc) {
          MultipleFPPhis = true;
          break;
        }
        Acc = &P;
      }
      if (!Acc || MultipleFPPhis)
        continue;
      if (!Acc->getType()->isDoubleTy())
        continue; // float accumulators: documented out of scope
      if (Acc->getNumIncomingValues() != 2)
        continue;

      // Init must be constant 0.0: the streaming state (m=-inf, s=0)
      // represents exactly "the sum of terms so far"; a nonzero init would
      // need a log_add of the incoming value, out of scope here.
      auto *Init = dyn_cast<ConstantFP>(Acc->getIncomingValueForBlock(Preheader));
      if (!Init || !Init->isZero())
        continue;

      // Backedge update: plain fadd(phi, X), inside the loop.
      auto *Upd = dyn_cast<BinaryOperator>(Acc->getIncomingValueForBlock(Latch));
      if (!Upd || Upd->getOpcode() != Instruction::FAdd || !L->contains(Upd))
        continue;
      Value *X = nullptr;
      if (Upd->getOperand(0) == Acc && Upd->getOperand(1) != Acc)
        X = Upd->getOperand(1);
      else if (Upd->getOperand(1) == Acc && Upd->getOperand(0) != Acc)
        X = Upd->getOperand(0);
      if (!X)
        continue;

      // X must be the exp call, through nothing but fp casts.
      while (auto *Cast = dyn_cast<CastInst>(X)) {
        if (Cast->getOpcode() != Instruction::FPExt &&
            Cast->getOpcode() != Instruction::FPTrunc)
          break;
        X = Cast->getOperand(0);
      }
      auto *ExpCall = dyn_cast<CallBase>(X);
      Value *Arg = nullptr;
      if (!ExpCall || !L->contains(ExpCall))
        continue;
      ExpKind EK = classifyExpCall(ExpCall, Arg);
      // A source-level exp/expf is the errno case: the exact shape this
      // pass targets, declined because replacing it would change observable
      // errno behaviour. Worth saying out loud — silence here would read as
      // "shape not recognized", which is the wrong diagnosis and would send
      // the next reader hunting the matcher instead of the build flags.
      // Fix at the call site: compile with -fno-math-errno.
      if (EK == ExpKind::ExternalExp) {
        errs() << "DECLINE-ERRNO,";
        printLoc(errs(), Upd, F);
        errs() << ",external-exp-call\n";
        continue;
      }
      if (EK != ExpKind::Intrinsic)
        continue;

      // The exp argument must be loop-varying — an invariant argument is a
      // hoistable constant sum, not the reduction this project targets.
      auto *ArgI = dyn_cast<Instruction>(Arg);
      if (!ArgI || !L->contains(ArgI) || ArgI == Acc)
        continue;

      // Mid-loop-read guard (matcher discipline): the phi's only in-loop
      // user is the update, and the update's only in-loop user is the phi.
      if (soleInLoopUser(Acc, *L) != Upd)
        continue;
      if (soleInLoopUser(Upd, *L) != Acc)
        continue;

      // Out-of-loop users of the running sum (phi) and the final sum (upd).
      SmallVector<Instruction *, 4> PhiOut, UpdOut;
      for (User *U : Acc->users())
        if (auto *UI = dyn_cast<Instruction>(U))
          if (!L->contains(UI))
            PhiOut.push_back(UI);
      for (User *U : Upd->users())
        if (auto *UI = dyn_cast<Instruction>(U))
          if (!L->contains(UI))
            UpdOut.push_back(UI);
      if (PhiOut.empty() && UpdOut.empty())
        continue; // sum unobservable; nothing to do

      // The final-value replacement is built on the exit edge and reads the
      // updated state (newm/snext, inserted next to Upd) — legal only if
      // Upd dominates the exit branch. True for rotated loops (update in
      // the exiting latch); decline otherwise.
      bool UpdDom = DT.dominates(Upd, ExitingBB->getTerminator());
      if (!UpdDom && !UpdOut.empty())
        continue;
      if (!UpdDom)
        continue; // keep the prototype to the one verified configuration

      // ---- Profitability gate (intent step 9) --------------------------
      // Shape is not profitability. The matcher study found the abundant
      // hits are benign-range dot products where a log rewrite only costs
      // speed, and that the rescue-worthy subset is small; the risk verdict
      // is the gate that separates them. Same rule as the matcher's:
      // exp-family factor in the term chain => HIGH.
      //
      // For the one shape this prototype matches the verdict is HIGH by
      // construction — the match REQUIRES an exp call — so this gate cannot
      // decline any loop the pass can currently rewrite. That is stated
      // rather than dressed up: the mechanism is here, wired, and logged,
      // and it starts doing real work the moment shape coverage widens to
      // the fmuladd and w[i]*exp(t) forms the matcher already recognizes at
      // MED and LOW. MinRisk exists so both branches are reachable and
      // testable today.
      const Risk R = Risk::High; // exp call in the chain, established above
      if (static_cast<int>(R) < static_cast<int>(MinRisk)) {
        errs() << "DECLINE-RISK,";
        printLoc(errs(), Upd, F);
        errs() << "," << riskName(R) << ",below-min-" << riskName(MinRisk)
               << "\n";
        continue;
      }

      // ---- All checks passed: build the streaming logsumexp state. ----
      Type *DTy = Acc->getType();
      IRBuilder<> HB(Header, Header->begin());
      PHINode *MPhi = HB.CreatePHI(DTy, 2, "lr.m");
      PHINode *SPhi = HB.CreatePHI(DTy, 2, "lr.s");
      MPhi->addIncoming(ConstantFP::getInfinity(DTy, /*Negative=*/true),
                        Preheader);
      SPhi->addIncoming(ConstantFP::get(DTy, 0.0), Preheader);

      IRBuilder<> B(Upd); // insert just before the (now-parallel) fadd
      Value *T = Arg;
      if (T->getType() != DTy)
        T = B.CreateFPExt(T, DTy, "lr.t"); // expf(float) case
      Value *NewM =
          B.CreateBinaryIntrinsic(Intrinsic::maxnum, MPhi, T, {}, "lr.newm");
      // Infinity guard. The exponents are differences against the running
      // max, and x - x is NaN when x is infinite: m = t = -inf (a zero term
      // arriving while the max is still -inf) and t = +inf both produce
      // inf - inf. Replace the difference by 0.0 exactly when the operand
      // already equals newm, which is the mathematically correct exponent in
      // every finite case too (x - x == 0). `oeq` is deliberate: a NaN t is
      // never equal to newm, so the subtract survives, exp(NaN) = NaN, and
      // NaN still poisons s — the linear loop's propagation is preserved.
      // MPhi is never NaN (maxnum(-inf, NaN) = -inf), so its guard only ever
      // fires on genuine equality.
      auto guardedSub = [&](Value *V, const char *Name) -> Value * {
        Value *Eq = B.CreateFCmpOEQ(V, NewM, Twine(Name) + ".eq");
        return B.CreateSelect(Eq, ConstantFP::get(DTy, 0.0),
                              B.CreateFSub(V, NewM, Twine(Name) + ".raw"),
                              Name);
      };
      Value *E1 = B.CreateUnaryIntrinsic(Intrinsic::exp,
                                         guardedSub(MPhi, "lr.dm"), {}, "lr.e1");
      Value *E2 = B.CreateUnaryIntrinsic(Intrinsic::exp,
                                         guardedSub(T, "lr.dt"), {}, "lr.e2");
      Value *SNext =
          B.CreateFAdd(B.CreateFMul(SPhi, E1, "lr.srescale"), E2, "lr.snext");
      MPhi->addIncoming(NewM, Latch);
      SPhi->addIncoming(SNext, Latch);

      // Dedicated landing block on the exit edge. NOTE: on a critical exit
      // edge SplitEdge preserves LCSSA by inserting pass-through phis for
      // loop values into the new block and retargeting the old exit's phis
      // to them — so user lists snapshotted before the split go stale. (The
      // first version of this pass hit exactly that: the linear replacement
      // was silently never wired in, caught because the benign-case
      // relative error was exactly 0 — bitwise-identical results.) All user
      // redirection therefore happens strictly after the split, below.
      BasicBlock *ReplBB =
          SplitEdge(ExitingBB, ExitBB, &DT, &LI, nullptr, "lr.exit");
      IRBuilder<> XB(ReplBB->getTerminator());
      Value *LogSum = XB.CreateFAdd(
          NewM, XB.CreateUnaryIntrinsic(Intrinsic::log, SNext, {}, "lr.logs"),
          "lr.logsum");
      Value *ReplFinal =
          XB.CreateUnaryIntrinsic(Intrinsic::exp, LogSum, {}, "lr.sum");
      // Running-sum (phi) users see the state *before* this iteration's
      // update: rebuilt from the phis, which always dominate the exit.
      // Built lazily — only if such a user actually exists.
      Value *ReplRunning = nullptr;
      auto runningRepl = [&]() -> Value * {
        if (!ReplRunning) {
          Value *LogSumP = XB.CreateFAdd(
              MPhi,
              XB.CreateUnaryIntrinsic(Intrinsic::log, SPhi, {}, "lr.logs.p"),
              "lr.logsum.p");
          ReplRunning =
              XB.CreateUnaryIntrinsic(Intrinsic::exp, LogSumP, {}, "lr.sum.p");
        }
        return ReplRunning;
      };

      // Snapshot the post-split out-of-loop consumers before any rewiring.
      // SplitEdge may have inserted pass-through phis; follow those (and any
      // fp trunc/ext cast in front of the leaf) to the IR instruction that
      // actually consumes the sum. This spike records that shape only; it does
      // not rewrite the consumer.
      SmallVector<ConsumerUse, 4> ConsumerUses;
      SmallPtrSet<Value *, 8> SeenVals;
      SmallPtrSet<Instruction *, 8> SeenUsers;
      collectConsumerUses(Upd, *L, /*IsFinalSum=*/true, ConsumerUses, SeenVals,
                          SeenUsers);
      collectConsumerUses(Acc, *L, /*IsFinalSum=*/false, ConsumerUses, SeenVals,
                          SeenUsers);

      // Fold the split-created pass-through phis into the replacement.
      for (PHINode &P : make_early_inc_range(ReplBB->phis())) {
        bool AllUpd = P.getNumIncomingValues() > 0;
        bool AllAcc = AllUpd;
        for (Value *IV : P.incoming_values()) {
          AllUpd &= (IV == Upd);
          AllAcc &= (IV == Acc);
        }
        if (AllUpd) {
          P.replaceAllUsesWith(ReplFinal);
          P.eraseFromParent();
        } else if (AllAcc) {
          P.replaceAllUsesWith(runningRepl());
          P.eraseFromParent();
        }
      }
      // Redirect any remaining out-of-loop users still referencing the
      // originals (direct uses, or LCSSA phis on a non-critical exit edge
      // where SplitEdge inserts no pass-through phi). ReplFinal/ReplRunning
      // dominate every such use: all paths out of the loop run through
      // ReplBB.
      SmallVector<Instruction *, 4> StaleU, StaleP;
      for (User *U : Upd->users())
        if (auto *UI = dyn_cast<Instruction>(U))
          if (!L->contains(UI))
            StaleU.push_back(UI);
      for (User *U : Acc->users())
        if (auto *UI = dyn_cast<Instruction>(U))
          if (!L->contains(UI))
            StaleP.push_back(UI);
      for (Instruction *U : StaleU)
        U->replaceUsesOfWith(Upd, ReplFinal);
      for (Instruction *U : StaleP)
        U->replaceUsesOfWith(Acc, runningRepl());

      // Export hook: store the log-domain final value. External declaration
      // created on demand; the consuming link must provide the definition.
      Module *M = F.getParent();
      Constant *G = M->getOrInsertGlobal(ExportGlobalName, DTy);
      XB.CreateStore(LogSum, G);

      // The original phi/fadd/exp chain is left in place, now feeding only
      // itself; later DCE/ADCE may remove it. Deliberate: this pass adds
      // and redirects, it does not delete (prototype simplicity).

      // Verdict is part of the record: a rewrite that fired should say what
      // profitability signal let it through, not just that it happened.
      errs() << "REWRITE,";
      printLoc(errs(), Upd, F);
      errs() << "," << riskName(R) << ",exp-chain;exp-sum\n";
      for (const ConsumerUse &CU : ConsumerUses) {
        const char *Kind = classifyConsumerUse(CU, *L);
        bool IsMatch = StringRef(Kind) == "fdiv-of-sum";
        if (!PropagateDiv) {
          // Default: log observed shape, no rewrite.
          errs() << (IsMatch ? "CONSUMER-MATCH," : "CONSUMER-DECLINE,");
          printLoc(errs(), CU.User, F);
          errs() << "," << Kind << "\n";
        } else if (!IsMatch) {
          // propagate=div enabled but structural clause failed.
          errs() << "DECLINE-PROP,";
          printLoc(errs(), CU.User, F);
          errs() << "," << Kind << "\n";
        } else {
          // propagate=div enabled, structural clauses passed: check domination
          // then emit exp(T - LogSum), where T is the pre-exp argument of an
          // llvm.exp(T) numerator.  Arbitrary numerators are declined:
          //   - a negative numerator would give log(negative) = NaN rather
          //     than a negative quotient;
          //   - the transform must not rely on a downstream InstCombine fold
          //     of log(exp(t)) -> t to be correct.
          auto *FDiv = cast<BinaryOperator>(CU.User);
          if (!DT.dominates(ReplBB, FDiv->getParent())) {
            errs() << "DECLINE-PROP,";
            printLoc(errs(), FDiv, F);
            errs() << ",not-dominated\n";
            continue;
          }
          // Peel fp casts to reach the potential llvm.exp call.
          // Only FPExt/FPTrunc are peeled: the same constraint as the loop
          // accumulation, where the loop-body exp argument passes through
          // nothing but fp casts.  A non-fp cast terminates the peel and
          // produces a decline (dyn_cast<CallBase> on the cast fails).
          Value *NumV = FDiv->getOperand(0);
          while (auto *Cast = dyn_cast<CastInst>(NumV)) {
            if (Cast->getOpcode() != Instruction::FPExt &&
                Cast->getOpcode() != Instruction::FPTrunc)
              break;
            NumV = Cast->getOperand(0);
          }
          Value *ExpArg = nullptr;
          bool NumIsIntrinsicExp = false;
          if (auto *NumCall = dyn_cast<CallBase>(NumV))
            NumIsIntrinsicExp =
                (classifyExpCall(NumCall, ExpArg) == ExpKind::Intrinsic);
          if (!NumIsIntrinsicExp) {
            errs() << "DECLINE-PROP,";
            printLoc(errs(), FDiv, F);
            errs() << ",numerator-not-exp\n";
            continue;
          }
          IRBuilder<> PB(FDiv);
          Value *T = ExpArg;
          // classifyExpCall accepts only float/double args, so T is at most
          // float when FDiv is double.  Use CreateFPCast to handle both
          // directions safely.
          if (T->getType() != FDiv->getType())
            T = PB.CreateFPCast(T, FDiv->getType(), "lr.t");
          Value *Diff = PB.CreateFSub(T, LogSum, "lr.diff");
          Value *Result = PB.CreateUnaryIntrinsic(Intrinsic::exp, Diff, {},
                                                  "lr.pdiv");
          errs() << "PROPAGATE,";
          printLoc(errs(), FDiv, F);
          errs() << "\n";
          FDiv->replaceAllUsesWith(Result);
          FDiv->eraseFromParent();
        }
      }
      Changed = true;
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LogRewrite", "0.1", [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  // Parameters, SEMICOLON-separated inside <>: force, and
                  // min-risk=high|med|low (default high). Semicolons, not
                  // commas — the new-PM pipeline parser splits on commas at
                  // the top level, so 'log-rewrite<force,min-risk=none>'
                  // reaches this callback as the pass name
                  // 'log-rewrite<force' and is rejected. min-risk is the
                  // profitability gate; see the note at the gate itself for
                  // why raising it above HIGH is currently the only way to
                  // make it decline.
                  if (!Name.consume_front("log-rewrite"))
                    return false;
                  bool Force = false;
                  Risk MinRisk = Risk::High;
                  bool PropagateDiv = false;
                  if (!Name.empty()) {
                    if (!Name.consume_front("<") || !Name.consume_back(">"))
                      return false;
                    while (!Name.empty()) {
                      StringRef Tok = Name.take_until([](char C) { return C == ';'; });
                      Name = Name.drop_front(Tok.size());
                      Name.consume_front(";");
                      if (Tok == "force")
                        Force = true;
                      else if (Tok == "min-risk=high")
                        MinRisk = Risk::High;
                      else if (Tok == "min-risk=med")
                        MinRisk = Risk::Med;
                      else if (Tok == "min-risk=low")
                        MinRisk = Risk::Low;
                      else if (Tok == "min-risk=none")
                        MinRisk = Risk::None;
                      else if (Tok == "propagate=div")
                        PropagateDiv = true;
                      else
                        return false; // unknown parameter: refuse, do not ignore
                    }
                  }
                  FPM.addPass(LogRewritePass(Force, MinRisk, PropagateDiv));
                  return true;
                });
          }};
}
