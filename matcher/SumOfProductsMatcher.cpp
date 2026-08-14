// SumOfProductsMatcher.cpp — LLVM opt plugin. RECOGNITION ONLY, no rewriting
// (intent Deliverable 2 precondition: measure the hit rate before building
// any transform). Match criteria are fixed in METHODOLOGY.md; this file
// implements them and nothing more.
//
// Emits greppable lines on stderr, one per event:
//   LOOP,<file>,<line>,<function>                       innermost FP loop examined
//   HIT,<file>,<line>,<function>,<trip>,<depth>,<nmul>,<transcendental|plain>,<risk>,<reasons>
// risk is the profitability signal — the gate in front of any rewrite:
// "would this reduction actually underflow?" HIGH | MED | LOW, with
// semicolon-joined reason tokens (or "none"). Aggregation happens in the
// run script, not here.
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

// Trace the additive spine from the backedge value down to the accumulator
// phi: the update may be a tree of fadd/fsub/fneg whose leaves are the phi
// and the reduction terms, and at -O1 clang folds "acc += a*b" into
// llvm.fmuladd(a, b, acc) — the phi then sits in the addend slot. Collects
// every term (non-spine operand) and counts spine fmuladds as multiplies.
// fsub only continues through its left operand: fsub(term, acc) alternates
// the accumulator's sign each iteration and is not a reduction.
// Nodes are appended to Spine only on the successful path (deepest first).
bool spineToPhi(Value *V, PHINode *Phi, const Loop &L,
                SmallVectorImpl<Value *> &Terms,
                SmallVectorImpl<Instruction *> &Spine, unsigned &SpineMuls,
                unsigned Depth = 8) {
  if (V == Phi) return true;
  if (Depth == 0) return false;
  auto *I = dyn_cast<Instruction>(V);
  if (!I || !L.contains(I)) return false;

  switch (I->getOpcode()) {
  case Instruction::FAdd:
    for (unsigned a = 0; a < 2; ++a)
      if (spineToPhi(I->getOperand(a), Phi, L, Terms, Spine, SpineMuls,
                     Depth - 1)) {
        Terms.push_back(I->getOperand(1 - a));
        Spine.push_back(I);
        return true;
      }
    return false;
  case Instruction::FSub:
    if (spineToPhi(I->getOperand(0), Phi, L, Terms, Spine, SpineMuls,
                   Depth - 1)) {
      Terms.push_back(I->getOperand(1));
      Spine.push_back(I);
      return true;
    }
    return false;
  case Instruction::FNeg:
    return false; // -acc as the running value flips sign: not a reduction
  case Instruction::Call:
    if (auto *II = dyn_cast<IntrinsicInst>(I)) {
      if (II->getIntrinsicID() == Intrinsic::fmuladd ||
          II->getIntrinsicID() == Intrinsic::fma) {
        if (spineToPhi(II->getArgOperand(2), Phi, L, Terms, Spine, SpineMuls,
                       Depth - 1)) {
          ++SpineMuls;
          Terms.push_back(II->getArgOperand(0));
          Terms.push_back(II->getArgOperand(1));
          Spine.push_back(I);
          return true;
        }
      }
    }
    return false;
  default:
    return false;
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

      // Candidate accumulators: FP phis in the header whose backedge value
      // is an in-loop fadd/fsub with the phi as one operand.
      for (PHINode &Phi : L->getHeader()->phis()) {
        if (!Phi.getType()->isFloatingPointTy()) continue;
        Value *Back = Phi.getIncomingValueForBlock(Latch);
        auto *Upd = dyn_cast<Instruction>(Back);
        if (!Upd || !L->contains(Upd)) continue;

        SmallVector<Value *, 8> Terms;
        SmallVector<Instruction *, 8> Spine;
        unsigned SpineMuls = 0;
        if (!spineToPhi(Upd, &Phi, *L, Terms, Spine, SpineMuls)) continue;

        // Every running value of the accumulator — the phi AND each spine
        // node — must have exactly one in-loop user (its consumer on the
        // spine; the root's consumer is the phi itself). Any other in-loop
        // user is a mid-loop read of the running sum, which a log rewrite
        // would change (e.g. prefix-sum stores).
        auto soleInLoopUser = [&](const Value *V2) -> const User * {
          const User *Found = nullptr;
          for (const User *U : V2->users()) {
            auto *UI = dyn_cast<Instruction>(U);
            if (UI && L->contains(UI)) {
              if (Found) return nullptr; // more than one
              Found = U;
            }
          }
          return Found;
        };
        bool cleanUses = soleInLoopUser(&Phi) != nullptr;
        for (size_t s = 0; cleanUses && s < Spine.size(); ++s) {
          const User *U = soleInLoopUser(Spine[s]);
          // Deepest-first order: consumer of Spine[s] is Spine[s+1], and the
          // root's consumer is the phi.
          const User *Expected =
              (s + 1 < Spine.size()) ? cast<User>(Spine[s + 1])
                                     : cast<User>(&Phi);
          cleanUses = (U == Expected);
        }
        if (!cleanUses) continue;

        ChainInfo CI;
        CI.nMul = SpineMuls;
        SmallPtrSet<Value *, 32> Visited;
        for (Value *T : Terms) walkChain(T, *L, CI, Visited);
        if (!CI.ok || CI.nMul == 0) continue; // plain sum or dirty chain: miss

        const char *Trip = "unknown";
        if (SE.getSmallConstantTripCount(L) > 0) Trip = "constant";
        else if (SE.hasLoopInvariantBackedgeTakenCount(L)) Trip = "runtime";

        // Profitability triage (the gate in front of any rewrite): would
        // this reduction actually underflow? HIGH iff an exp-family factor
        // is in the chain (unbounded magnitude); MED for many multiplied
        // factors, or log-domain inputs multiplied together; else LOW.
        bool deepChain = CI.nMul >= 4;
        bool unknownTrip = StringRef(Trip) == "unknown";
        const char *Risk = CI.expChain ? "HIGH"
                           : (deepChain || (CI.logChain && CI.nMul >= 2))
                               ? "MED"
                               : "LOW";
        SmallVector<const char *, 4> Reasons;
        if (CI.expChain) Reasons.push_back("exp-chain");
        if (CI.logChain) Reasons.push_back("log-chain");
        if (deepChain) Reasons.push_back("deep-chain");
        if (unknownTrip) Reasons.push_back("unknown-trip");

        errs() << "HIT,";
        printLoc(errs(), Upd, F);
        errs() << "," << Trip << "," << CI.depth << "," << CI.nMul << ","
               << (CI.transcendental ? "transcendental" : "plain") << ","
               << Risk << ",";
        if (Reasons.empty())
          errs() << "none";
        else
          for (size_t i = 0; i < Reasons.size(); ++i)
            errs() << (i ? ";" : "") << Reasons[i];
        errs() << "\n";
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
