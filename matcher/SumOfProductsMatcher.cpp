// SumOfProductsMatcher.cpp — LLVM opt plugin. RECOGNITION ONLY, no rewriting
// (intent Deliverable 2 precondition: measure the hit rate before building
// any transform). Match criteria are fixed in METHODOLOGY.md; this file
// implements them and nothing more.
//
// Emits greppable lines on stderr, one per event:
//   LOOP,<file>,<line>,<function>                       innermost FP loop examined
//   HIT,<file>,<line>,<function>,<trip>,<depth>,<nmul>,<transcendental>
// Aggregation happens in the run script, not here.
//
// Usage: opt -load-pass-plugin=./SopMatcher.so -passes=sop-matcher \
//            -disable-output module.bc

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

// Names whose presence in the product chain marks likelihood-style math —
// the exact shape the project targets. Matched by substring on the callee.
bool isTranscendentalName(StringRef Name) {
  return Name.contains("exp") || Name.contains("log") ||
         Name.contains("pow") || Name.contains("sqrt");
}

struct ChainInfo {
  unsigned depth = 0;          // instructions visited in the term chain
  unsigned nMul = 0;           // fmul / fmuladd count
  bool transcendental = false; // readonly exp/log/pow/sqrt calls in chain
  bool ok = true;              // chain stayed within the allowed op set
};

// Walk the definition chain of the reduction term (the non-accumulator
// operand of the update), staying inside the loop. Allowed interior ops per
// METHODOLOGY.md: fmul, fadd, fsub, fneg, fp casts, int->fp casts, loads,
// selects, and calls to read-only FP functions. Anything else (stores,
// side-effecting calls, another phi — a nested reduction) fails the chain.
void walkChain(Value *V, const Loop &L, ChainInfo &CI,
               SmallPtrSetImpl<Value *> &Visited, unsigned Budget = 64) {
  if (!CI.ok || CI.depth > Budget) { CI.ok = CI.depth <= Budget; return; }
  auto *I = dyn_cast<Instruction>(V);
  if (!I || !L.contains(I)) return; // loop-invariant / constant / argument: leaf
  if (!Visited.insert(V).second) return;
  ++CI.depth;

  switch (I->getOpcode()) {
  case Instruction::FMul:
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
    if (CB->onlyReadsMemory() && !CB->isIndirectCall()) {
      if (Function *Callee = CB->getCalledFunction())
        if (isTranscendentalName(Callee->getName())) CI.transcendental = true;
      for (Use &U : CB->args()) walkChain(U.get(), L, CI, Visited, Budget);
      return;
    }
    CI.ok = false;
    return;
  }
  default:
    CI.ok = false;
    return;
  }
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
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);

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

      Instruction *Anchor = &*L->getHeader()->begin();
      errs() << "LOOP,";
      printLoc(errs(), Anchor, F);
      errs() << "\n";

      BasicBlock *Latch = L->getLoopLatch();
      if (!Latch) continue; // irregular loop: cannot be a simple reduction

      // Candidate accumulators: FP phis in the header whose backedge value
      // is an in-loop fadd/fsub with the phi as one operand.
      for (PHINode &Phi : L->getHeader()->phis()) {
        if (!Phi.getType()->isFloatingPointTy()) continue;
        Value *Back = Phi.getIncomingValueForBlock(Latch);
        auto *Upd = dyn_cast<BinaryOperator>(Back);
        if (!Upd || !L->contains(Upd)) continue;
        if (Upd->getOpcode() != Instruction::FAdd &&
            Upd->getOpcode() != Instruction::FSub)
          continue;
        Value *Term = nullptr;
        if (Upd->getOperand(0) == &Phi)      Term = Upd->getOperand(1);
        else if (Upd->getOperand(1) == &Phi &&
                 Upd->getOpcode() == Instruction::FAdd)
          Term = Upd->getOperand(0); // fsub(term, acc) is not a reduction
        if (!Term) continue;

        // The accumulator's only in-loop user must be its own update —
        // a mid-loop read would change semantics under a log rewrite.
        bool cleanUses = true;
        for (User *U : Phi.users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (UI && L->contains(UI) && UI != Upd) { cleanUses = false; break; }
        }
        if (!cleanUses) continue;

        ChainInfo CI;
        SmallPtrSet<Value *, 32> Visited;
        walkChain(Term, *L, CI, Visited);
        if (!CI.ok || CI.nMul == 0) continue; // plain sum or dirty chain: miss

        const char *Trip = "unknown";
        if (SE.getSmallConstantTripCount(L) > 0) Trip = "constant";
        else if (SE.hasLoopInvariantBackedgeTakenCount(L)) Trip = "runtime";

        errs() << "HIT,";
        printLoc(errs(), Upd, F);
        errs() << "," << Trip << "," << CI.depth << "," << CI.nMul << ","
               << (CI.transcendental ? "transcendental" : "plain") << "\n";
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
                    FPM.addPass(SopMatcherPass());
                    return true;
                  }
                  return false;
                });
          }};
}
