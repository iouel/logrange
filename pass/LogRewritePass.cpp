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
//   - X is a call to exp/expf/llvm.exp, possibly through fpext/fptrunc only
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
//            snext = s * llvm.exp(m - newm) + llvm.exp(t - newm)
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
// Known caveat: t=-inf on the first iteration gives -inf - -inf = NaN
// (documented in PROTOTYPE.md, not solved here).
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
// Emits one line per rewrite on stderr:  REWRITE,<file>,<line>,<function>
//
// Usage: opt-21 -load-pass-plugin=./LogRewrite.so \
//               -passes='log-rewrite<force>' -S in.ll -o out.ll

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

// Accept exp as libm call (exp/expf; may write errno — irrelevant to shape,
// same stance as the matcher's isKnownLibmCall) or as the llvm.exp
// intrinsic. Scalar float/double argument only.
bool isExpCall(CallBase *CB, Value *&ArgOut) {
  Value *A = nullptr;
  if (auto *II = dyn_cast<IntrinsicInst>(CB)) {
    if (II->getIntrinsicID() != Intrinsic::exp)
      return false;
    A = II->getArgOperand(0);
  } else {
    if (CB->isIndirectCall())
      return false;
    Function *Callee = CB->getCalledFunction();
    if (!Callee)
      return false;
    StringRef N = Callee->getName();
    if (N != "exp" && N != "expf")
      return false;
    if (CB->arg_size() != 1)
      return false;
    A = CB->getArgOperand(0);
  }
  if (!A->getType()->isFloatTy() && !A->getType()->isDoubleTy())
    return false;
  ArgOut = A;
  return true;
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

struct LogRewritePass : PassInfoMixin<LogRewritePass> {
  bool Force;
  explicit LogRewritePass(bool Force) : Force(Force) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    // Opt-in gate. Being named in -passes got us here at all; on top of
    // that, reassociation legality must be granted by the caller: either
    // the function is already marked unsafe-fp-math (e.g. -ffast-math) or
    // the pass was instantiated with the force parameter.
    bool OptIn =
        Force || F.getFnAttribute("unsafe-fp-math").getValueAsString() == "true";
    if (!OptIn)
      return PreservedAnalyses::all();

    auto &LI = AM.getResult<LoopAnalysis>(F);
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
      if (!ExpCall || !L->contains(ExpCall) || !isExpCall(ExpCall, Arg))
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
      Value *E1 = B.CreateUnaryIntrinsic(
          Intrinsic::exp, B.CreateFSub(MPhi, NewM, "lr.dm"), {}, "lr.e1");
      Value *E2 = B.CreateUnaryIntrinsic(
          Intrinsic::exp, B.CreateFSub(T, NewM, "lr.dt"), {}, "lr.e2");
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

      errs() << "REWRITE,";
      printLoc(errs(), Upd, F);
      errs() << "\n";
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
                  if (Name == "log-rewrite") {
                    FPM.addPass(LogRewritePass(/*Force=*/false));
                    return true;
                  }
                  if (Name == "log-rewrite<force>") {
                    FPM.addPass(LogRewritePass(/*Force=*/true));
                    return true;
                  }
                  return false;
                });
          }};
}
