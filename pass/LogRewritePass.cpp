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
//   - its backedge update is one of three spines: fadd(phi, X),
//     fadd(phi, fmul(W, X)), or llvm.fmuladd(W, X, phi)
//   - X is a call to llvm.exp.*, possibly through fpext/fptrunc only
//     (source-level exp/expf declined: errno — see the errno contract below)
//   - the call argument is loop-varying (an instruction inside the loop)
//   - the phi and the update have no other in-loop users (the matcher's
//     mid-loop-read guard: a prefix-sum-style read would change meaning)
//   - at least one out-of-loop user of the sum exists (else nothing
//     observable would change)
//
// Matched is not rewritten. Only the unweighted spine, fadd(phi, exp(t)), is
// rewritten. The two weighted spines are matched so they can be declined with
// a stated reason: the weight is not proven bounded, so the emitted state
// would reach sum|w_i| where the unweighted state is at most n (see the
// weight clause). A spine with neither a multiply nor an exp is not matched
// at all — the matcher's noMulNoExp rule.
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
//     -passes='loop-simplify,lcssa,log-rewrite<force>'
// The two canonicalization passes are REQUIRED (ELIGIBILITY.md section 0):
// recognition delegates to RecurrenceDescriptor, whose AddReductionVar reads
// through the preheader and inspects out-of-loop users. Without them every
// loop declines and the run reports nothing.
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
// Declines are logged too: DECLINE-FPENV,<file>,<line>,<fn>,<reason>,
// DECLINE-ERRNO,<file>,<line>,<fn>,external-exp-call,
// DECLINE-RISK,<file>,<line>,<fn>,<verdict>,below-min-<tier> and
// DECLINE-WEIGHT,<file>,<line>,<fn>,unbounded-weight.
//
// Usage: opt-21 -load-pass-plugin=./LogRewrite.so \
//               -passes='loop-simplify,lcssa,log-rewrite<force>,adce' \
//               -S in.ll -o out.ll

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/FloatingPointMode.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
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

#include <cmath>

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

// What a spine term ultimately is: strip fp-only casts, then classify the
// call underneath. ArgOut is written only when the term does reach an exp
// call. The walk is deliberately this short — casts and nothing else —
// because the accepted spines put nothing else between the update and the
// call, and a longer walk would be claiming coverage the rewrite does not
// have. It is not the matcher's general walkChain.
ExpKind classifyTerm(Value *V, const Loop &L, Value *&ArgOut) {
  while (auto *Cast = dyn_cast<CastInst>(V)) {
    if (Cast->getOpcode() != Instruction::FPExt &&
        Cast->getOpcode() != Instruction::FPTrunc)
      break;
    V = Cast->getOperand(0);
  }
  auto *CB = dyn_cast<CallBase>(V);
  if (!CB || !L.contains(CB))
    return ExpKind::No;
  return classifyExpCall(CB, ArgOut);
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

// Sole in-loop user.
//
// This is NOT the accumulator's clean-use discipline — isReductionPHI
// establishes that, and this pass stopped carrying its own copy on 2026-08-17.
// What remains is a check on a TERM-side node: the `fmul` in
// `fadd(phi, fmul(W, X))` is not part of the reduction chain, so no reduction
// analysis has an opinion about who else reads it. Kept for that one use.
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
    // Required by RecurrenceDescriptor::isReductionPHI. DB/AC drive its
    // minimal-bit-width computation (irrelevant for FP, but the API takes
    // them); SE is what lets it treat a store to a loop-invariant address as
    // part of the reduction rather than as a mid-loop read.
    auto &AC = AM.getResult<AssumptionAnalysis>(F);
    auto &DB = AM.getResult<DemandedBitsAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    bool Changed = false;

    // Snapshot: we add blocks (edge split) but never delete or restructure
    // existing loops, so the preorder walk stays valid.
    SmallVector<Loop *, 8> Loops(LI.getLoopsInPreorder());
    for (Loop *L : Loops) {
      if (!L->isInnermost())
        continue;
      BasicBlock *Header = L->getHeader();

      // Pipeline diagnosis runs FIRST, before the shape checks below.
      //
      // These are prerequisites of the ANALYSIS THIS PASS HAPPENS TO USE, not
      // conditions on when a log-domain rewrite is legal. isReductionPHI needs
      // loop-simplify form (AddReductionVar reads the start value through
      // getLoopPreheader() with no null check — an out-of-bounds read with
      // assertions off) and LCSSA (it inspects out-of-loop users to locate the
      // exit instruction). Swap the recognizer and this requirement changes
      // with it. ELIGIBILITY.md keeps it out of the numbered clauses for that
      // reason; nothing downstream should reason from it.
      //
      // Stated rather than skipped because a caller who omits the prefix gets
      // zero rewrites, and silence reads as "no eligible loop" rather than
      // "misconfigured pipeline". DECLINE-PIPELINE is a configuration
      // diagnostic, not an eligibility decline like DECLINE-WEIGHT.
      //
      // Order matters and was got wrong once. Placed after the
      // preheader/latch null checks below, this guard is unreachable for the
      // case it exists to name: an un-canonicalized loop has no preheader, so
      // it bails there and declines silently anyway. Measured on the test
      // kernel — bare pipeline, 0 rewrites and 0 declines.
      const char *PipeReason = nullptr;
      if (!L->isLoopSimplifyForm())
        PipeReason = "not-loop-simplified";
      else if (!L->isLCSSAForm(DT))
        PipeReason = "not-lcssa";
      if (PipeReason) {
        errs() << "DECLINE-PIPELINE,";
        printLoc(errs(), &*Header->begin(), F);
        errs() << "," << PipeReason << "\n";
        continue;
      }

      BasicBlock *Preheader = L->getLoopPreheader();
      BasicBlock *Latch = L->getLoopLatch();
      BasicBlock *ExitingBB = L->getExitingBlock(); // unique or null
      BasicBlock *ExitBB = L->getExitBlock();       // unique or null
      // Preheader and Latch are implied by loop-simplify form; the unique
      // exiting and exit blocks are not, and are this pass's own requirement.
      if (!Preheader || !Latch || !ExitingBB || !ExitBB)
        continue;

      // Exactly one FP phi in the header: the accumulator. A second FP phi
      // would be another loop-carried FP value interacting with the sum in
      // ways this prototype does not analyze. This is a REWRITE-legality
      // condition, not a recognition one — isReductionPHI is happy to
      // describe two independent reductions in one loop — so it stays.
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

      // STAGE 1 of 4: is it a reduction, and are its uses clean? LLVM's
      // answer, not this file's. Until 2026-08-17 the pass established this
      // itself, with a copy of the matcher's mid-loop-read guard and its own
      // reading of the phi's incoming values; matcher/DELTA.md measured what
      // that cost. FAdd and FMulAdd are the sum-shaped kinds — FMul is a
      // product recurrence, min/max are not sums, neither is rewritable here.
      //
      // THIS IS THE ONLY THING LLVM ESTABLISHES. It does not validate the
      // term decomposition below. `RecurKind::FMulAdd` says the update is
      // `fmuladd(a, b, phi)`; which of a and b is the weight and which is the
      // exp-bearing term is a question LLVM has no opinion about, and
      // splitProduct answers it on this project's own terms. Do not let a
      // RecurKind stand in for a decomposition having been checked.
      //
      // The four stages are kept separate on purpose (ELIGIBILITY.md 3.3):
      //   1. RecurrenceDescriptor — reduction candidate
      //   2. this pass           — candidate decomposes as exp(t) or W*exp(t)
      //   3. weight analysis     — W is provably acceptable
      //   4. rewrite             — actually consumes W
      // Collapsing 3 into 4 is what let `s += 0.5*exp(x)` be rewritten with
      // the 0.5 silently discarded while the whole suite stayed green.
      RecurrenceDescriptor RD;
      if (!RecurrenceDescriptor::isReductionPHI(Acc, L, RD, &DB, &AC, &DT, &SE))
        continue;
      const RecurKind RK = RD.getRecurrenceKind();
      if (RK != RecurKind::FAdd && RK != RecurKind::FMulAdd)
        continue;

      // Init must be constant 0.0: the streaming state (m=-inf, s=0)
      // represents exactly "the sum of terms so far"; a nonzero init would
      // need a log_add of the incoming value, out of scope here.
      Value *Start = RD.getRecurrenceStartValue(); // TrackingVH -> Value*
      auto *Init = dyn_cast_or_null<ConstantFP>(Start);
      if (!Init || !Init->isZero())
        continue;

      // Backedge update. Three spines are accepted, all of them "the phi plus
      // a term", differing only in whether the term carries a multiplicative
      // weight and whether that multiply was contracted into the add:
      //
      //   fadd(phi, X)             the unweighted spine, the one rewritten
      //   fadd(phi, fmul(W, X))    weighted, multiply not contracted
      //   llvm.fmuladd(W, X, phi)  weighted, multiply contracted
      //
      // Which of the last two clang emits is a flag the pass does not
      // control. Measured on pass/test_softmax.c, LLVM 21: -ffp-contract=on
      // (the default, and the harness's) gives llvm.fmuladd; -ffp-contract=off
      // and =fast both give fmul + fadd. Matching one form only would make
      // this pass's coverage a property of the caller's contraction setting.
      //
      // The weighted spines are matched in order to be DECLINED with a stated
      // reason rather than be invisible. Nothing weighted is rewritten.
      Instruction *Upd = RD.getLoopExitInstr();
      if (!Upd || !L->contains(Upd))
        continue;

      Value *X = nullptr;      // the term added to the accumulator
      Value *Weight = nullptr; // its multiplicative factor, or null

      // Of a product's two operands the term is the one that reaches an exp
      // and the weight is the other. With no exp on either side the split is
      // arbitrary and does not matter: that verdict is LOW and the risk gate
      // declines before either value is read again.
      auto splitProduct = [&](Value *A, Value *B) {
        Value *Ag = nullptr;
        if (classifyTerm(B, *L, Ag) != ExpKind::No) {
          X = B;
          Weight = A;
        } else {
          X = A;
          Weight = B;
        }
      };

      if (auto *FMA = dyn_cast<IntrinsicInst>(Upd)) {
        if (FMA->getIntrinsicID() != Intrinsic::fmuladd)
          continue;
        // The accumulator must be the addend. As a multiplicand it is a
        // product recurrence, not a sum.
        if (FMA->getArgOperand(2) != Acc || FMA->getArgOperand(0) == Acc ||
            FMA->getArgOperand(1) == Acc)
          continue;
        splitProduct(FMA->getArgOperand(0), FMA->getArgOperand(1));
      } else if (auto *BO = dyn_cast<BinaryOperator>(Upd)) {
        if (BO->getOpcode() != Instruction::FAdd)
          continue;
        Value *Other = nullptr;
        if (BO->getOperand(0) == Acc && BO->getOperand(1) != Acc)
          Other = BO->getOperand(1);
        else if (BO->getOperand(1) == Acc && BO->getOperand(0) != Acc)
          Other = BO->getOperand(0);
        if (!Other)
          continue;
        auto *FM = dyn_cast<BinaryOperator>(Other);
        if (FM && FM->getOpcode() == Instruction::FMul && L->contains(FM)) {
          // The fmul is a spine node, so it takes the same clean-use
          // discipline as the update: its only in-loop user must be the add.
          // A second reader observes the product mid-loop, and then this is
          // not the reduction it looks like.
          if (soleInLoopUser(FM, *L) != Upd)
            continue;
          if (FM->getOperand(0) == Acc || FM->getOperand(1) == Acc)
            continue;
          splitProduct(FM->getOperand(0), FM->getOperand(1));
        } else {
          X = Other;
        }
      } else {
        continue;
      }

      Value *Arg = nullptr;
      ExpKind EK = classifyTerm(X, *L, Arg);

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

      // The matcher's noMulNoExp rule, held to here so the two tools agree on
      // what is in scope at all. A reduction with neither a multiply nor an
      // exp is rescuable without logsumexp, so it is a labeled miss on both
      // sides rather than a decline carrying a verdict. plain_sum is the
      // named case (matcher/SumOfProductsMatcher.cpp, "noMulNoExp").
      if (!Weight && EK == ExpKind::No)
        continue;

      // The mid-loop-read guard that stood here — the phi's only in-loop user
      // is the update and vice versa — is established by isReductionPHI
      // above. AddReductionVar walks the chain and refuses any in-loop user
      // that is not part of it, which is the same property.

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

      // ---- Profitability gate (intent Shipping Posture) ----------------
      // Shape is not profitability. The matcher study found the abundant
      // hits are benign-range dot products where a log rewrite only costs
      // speed, and that the rescue-worthy subset is small; the risk verdict
      // is the gate that separates them. The rule here: an accepted exp call
      // in the term => HIGH, because exp(t) spans the whole range from t
      // alone. Otherwise LOW.
      //
      // NOT the matcher's rule, and the difference is stated rather than
      // glossed. Two divergences, both measured:
      //   - The matcher's exp-family is exp/expm1/exp2/pow (substring match).
      //     This pass accepts only exp/expf/llvm.exp, so `s += pow(a,b)` is
      //     matcher-HIGH and a silent miss here, and `s += c*pow(a,b)` is
      //     matcher-HIGH while this gate prints LOW.
      //   - The matcher's MED needs nMul >= 4, or a log chain with nMul >= 2,
      //     counted over the WHOLE term chain by walkChain — including
      //     multiplies inside the exp argument, and counting fdiv. This pass
      //     counts spine multiplies only, which is a different quantity. So
      //     `s += a*b*c*d*e` is matcher-MED and prints LOW here.
      // MED is therefore unreachable in THIS function by construction (it
      // computes only Low and High), which is not the same claim as "no
      // matched loop can be graded MED". The matcher can and does grade some
      // of them MED.
      //
      // The gate declines a real input as of the fmuladd widening above:
      // dot_sum's llvm.fmuladd(x, y, phi) is matched, verdicts LOW, and is
      // refused at the default threshold.
      //
      // It is still NOT load-bearing, and that is asserted rather than
      // described. Eligibility below requires EK == Intrinsic, and the
      // verdict is HIGH exactly when EK != No, so every rewritable loop
      // verdicts HIGH; and every LOW loop is weighted (the noMulNoExp screen
      // guarantees Weight is non-null when EK == No), so the weight clause
      // refuses it at any threshold. The gate therefore changes which reason
      // token is printed, never whether a rewrite happens.
      // run_pass_test.sh pins this: the rewrite set at min-risk=low is
      // identical to the default one, and that assertion turns red when the
      // rewritable set finally exceeds the HIGH set.
      const Risk R = EK == ExpKind::No ? Risk::Low : Risk::High;
      if (static_cast<int>(R) < static_cast<int>(MinRisk)) {
        errs() << "DECLINE-RISK,";
        printLoc(errs(), Upd, F);
        errs() << "," << riskName(R) << ",below-min-" << riskName(MinRisk)
               << "\n";
        continue;
      }

      // ---- Weight clause ------------------------------------------------
      // A weighted term is matched and declined. Folding w_i into the term
      // makes the state accumulate sum(w_i * exp(t_i - m)) instead of
      // sum(exp(t_i - m)), and w_i is not proven bounded.
      //
      // MAGNITUDE is the durable reason. Every scaled term is at most |w_i|,
      // so the state reaches sum|w_i|, which has no ceiling — where the
      // unweighted state is at most n by construction, the exponents being
      // <= 0. Measured on the emitted state machine at n = 2:
      // w = (1e308, 1e308), t = (-700, -700) drives the state to inf and the
      // result to inf; the linear loop gives 19719.4. No reduction can
      // recover a state that already overflowed.
      //
      // SIGN is a second failure, and unlike magnitude it is a property of
      // the reduction rather than of the state: this pass emits
      // exp(m + log(s)), so w = (1, -2), t = (0, 0) drives the state to -1
      // and log of it to NaN where the linear loop gives -1. That one is
      // fixable — copysign(exp(m + log|s|), s) is correct for a negative sum
      // (matcher census, 2026-08-16, which retracted an earlier claim that
      // negative weights break semantics outright). It is recorded here as
      // work a weighted rewrite would have to do, not as the reason for the
      // decline.
      //
      // Ordered AFTER the risk gate, and that order is load-bearing. Reversed,
      // this clause shadows the gate: every weighted spine would decline here
      // and dot_sum, the only LOW-verdict input the pass matches, would never
      // reach the gate at all. Negative-tested by making the gate skip
      // weighted spines: dot_sum logs DECLINE-WEIGHT and the gate assertion
      // in run_pass_test.sh fails.
      //
      // EXTENSION POINT, deliberately not taken. A weight proven bounded —
      // a constant is the easy case — is rewritable. It needs the sign
      // handling above, the error contract re-derived (ELIGIBILITY.md's bound
      // is pos_accum's, for unit weights), and its own accept and decline
      // tests. Until those exist, every weight declines.
      //
      // The yield evidence for NOT building it is thin, and was published
      // worse than thin: 0 w*exp(t) multiplies, but among the 5 exp-carrying
      // reductions the matcher accepted, not among 2859 loops. The census is
      // gated on expChain and runs after the HIT, so it cannot see loops
      // rejected upstream — including the mirrored out[j] += w*exp(t) form.
      // See ELIGIBILITY.md 3.3; derivation: matcher/run_study.sh figures.
      if (Weight) {
        errs() << "DECLINE-WEIGHT,";
        printLoc(errs(), Upd, F);
        errs() << ",unbounded-weight\n";
        continue;
      }

      // Implied by the two clauses above rather than a live branch: an
      // unweighted spine reaches here only with an exp term (noMulNoExp), and
      // the external form already declined.
      if (EK != ExpKind::Intrinsic)
        continue;

      // The exp argument must be loop-varying — an invariant argument is a
      // hoistable constant sum, not the reduction this project targets.
      auto *ArgI = dyn_cast<Instruction>(Arg);
      if (!ArgI || !L->contains(ArgI) || ArgI == Acc)
        continue;

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
      // itself. Deliberate: this pass adds and redirects, it does not delete
      // (prototype simplicity).
      //
      // Removing it requires ADCE specifically, not DCE. The orphan is a
      // loop-carried cycle — the phi feeds the update and the update feeds the
      // phi — so every instruction in it has a use and plain DCE cannot start.
      // Measured on the test kernel: log-rewrite alone leaves 26 llvm.exp.f64
      // calls, `-passes='log-rewrite<force>,dce'` still 26, and
      // `,adce` 22, one dead exp per rewritten f64 loop. The supported
      // pipeline is therefore log-rewrite followed by adce, asserted in
      // run_pass_test.sh.

      // Verdict is part of the record: a rewrite that fired should say what
      // profitability signal let it through, not just that it happened.
      errs() << "REWRITE,";
      printLoc(errs(), Upd, F);
      // The reason list is a CONSTANT, not a computed taxonomy, and does not
      // track the matcher's definitions. The matcher emits exp-sum only when
      // nMul == 0 over the whole term chain, so for `s += exp(a*b*c*d)` it
      // prints exp-chain alone while this line prints exp-chain;exp-sum.
      // Every rewritten shape does carry an exp in the term, so exp-chain is
      // always correct here; exp-sum is not. Computing it needs the chain
      // walk this pass deliberately does not have (see classifyTerm).
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
          // propagate=div enabled, structural clauses passed.
          //
          // Two things have to be true at the divide, and the first version of
          // this code got both wrong.
          //
          // (1) A log form of the DIVISOR must exist on every path reaching
          //     the divide. Requiring ReplBB to dominate the divide is too
          //     strong: clang guards the accumulation loop, so the divisor the
          //     consumer sees is an LCSSA phi merging the rewritten sum with
          //     the constant 0.0 from the zero-trip path, and no single block
          //     dominates. The log form has to travel through that merge, so
          //     build it: a value's log form is LogSum for the rewritten sum,
          //     log(c) for a constant (0.0 -> -inf), and for a phi a parallel
          //     phi of its operands' log forms. Anything else declines. That
          //     is the lattice's phi transfer function and nothing more.
          //
          // (2) The NUMERATOR must not be re-logged. exp(t)/s becomes
          //     exp(t - L) using t directly. Taking log(numerator) would
          //     compute log(exp(t)), and exp(t) is exactly the quantity that
          //     underflows to 0.0 in the regime this rewrite exists for —
          //     log(0) = -inf, and the propagated result would be 0 or NaN
          //     precisely where the rescue is the point.
          auto *FDiv = cast<BinaryOperator>(CU.User);

          DenseMap<Value *, Value *> LogOf;
          auto logForm = [&](auto &self, Value *V) -> Value * {
            if (V == ReplFinal)
              return LogSum;
            auto It = LogOf.find(V);
            if (It != LogOf.end())
              return It->second;
            if (auto *C = dyn_cast<ConstantFP>(V)) {
              const APFloat &A = C->getValueAPF();
              if (A.isNaN() || A.isNegative())
                return nullptr; // no real logarithm
              if (A.isZero())
                return ConstantFP::getInfinity(DTy, /*Negative=*/true);
              return ConstantFP::get(
                  DTy, std::log(A.convertToDouble()));
            }
            if (auto *P = dyn_cast<PHINode>(V)) {
              if (L->contains(P) || P->getType() != DTy)
                return nullptr;
              // Insert with the phis, and memoize BEFORE recursing so a
              // cyclic phi network terminates instead of recursing forever.
              IRBuilder<> PhiB(P->getParent(), P->getParent()->begin());
              PHINode *NewP = PhiB.CreatePHI(DTy, P->getNumIncomingValues(),
                                             "lr.logphi");
              LogOf[V] = NewP;
              for (unsigned I = 0, E = P->getNumIncomingValues(); I != E; ++I) {
                Value *In = self(self, P->getIncomingValue(I));
                if (!In) {
                  LogOf.erase(V);
                  NewP->eraseFromParent();
                  return nullptr;
                }
                NewP->addIncoming(In, P->getIncomingBlock(I));
              }
              return NewP;
            }
            return nullptr;
          };

          Value *LogDen = logForm(logForm, CU.SeenAs);
          if (!LogDen) {
            errs() << "DECLINE-PROP,";
            printLoc(errs(), FDiv, F);
            errs() << ",no-log-form\n";
            continue;
          }
          if (auto *LogDenI = dyn_cast<Instruction>(LogDen))
            if (!DT.dominates(LogDenI, FDiv)) {
              errs() << "DECLINE-PROP,";
              printLoc(errs(), FDiv, F);
              errs() << ",log-form-not-available\n";
              continue;
            }

          // Peel fp casts to reach an llvm.exp numerator; require one.
          Value *Num = FDiv->getOperand(0);
          while (auto *CI = dyn_cast<CastInst>(Num)) {
            if (CI->getOpcode() != Instruction::FPExt &&
                CI->getOpcode() != Instruction::FPTrunc)
              break;
            Num = CI->getOperand(0);
          }
          Value *ExpArg = nullptr;
          if (auto *II = dyn_cast<IntrinsicInst>(Num))
            if (II->getIntrinsicID() == Intrinsic::exp &&
                II->getArgOperand(0)->getType() == DTy)
              ExpArg = II->getArgOperand(0);
          if (!ExpArg) {
            errs() << "DECLINE-PROP,";
            printLoc(errs(), FDiv, F);
            errs() << ",numerator-not-exp\n";
            continue;
          }

          IRBuilder<> PB(FDiv);
          Value *Diff = PB.CreateFSub(ExpArg, LogDen, "lr.diff");
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
