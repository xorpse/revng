//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include "revng/Model/FunctionTags.h"
#include "revng/Support/Debug.h"
#include "revng/Support/IRBuilder.h"

using namespace llvm;

static constexpr const char *Flag = "segment-accesses-to-gep";

static Logger Log("segment-accesses-to-gep");

/// A segment address reaches the code as integer arithmetic on the segment
/// global: `inttoptr(add(ptrtoint(@segment), K))`. That form carries no type, so
/// `emit-field-accesses` cannot see through it to the struct describing the
/// segment, and the offset stays a bare number. Turning it into a GEP makes the
/// base pointer-typed again, which is what that pass needs to name the field.
struct SegmentAccessesToGEPPass : public llvm::FunctionPass {
public:
  static char ID;

public:
  SegmentAccessesToGEPPass() : FunctionPass(ID) {}

public:
  bool runOnFunction(llvm::Function &F) override;
};

/// Look through the width casts the lifter inserts around a segment address.
static Value *skipIntegerCasts(Value *V) {
  while (auto *Cast = dyn_cast<CastInst>(V)) {
    if (not isa<TruncInst>(Cast) and not isa<ZExtInst>(Cast)
        and not isa<SExtInst>(Cast))
      break;
    V = Cast->getOperand(0);
  }
  return V;
}

/// The segment global \p V is the address of, if it is one.
static GlobalVariable *segmentGlobalOf(Value *V) {
  V = skipIntegerCasts(V);

  if (auto *Expression = dyn_cast<ConstantExpr>(V)) {
    if (Expression->getOpcode() != Instruction::PtrToInt)
      return nullptr;
    V = Expression->getOperand(0);
  } else if (auto *ToInteger = dyn_cast<PtrToIntInst>(V)) {
    V = ToInteger->getOperand(0);
  } else {
    return nullptr;
  }

  auto *Global = dyn_cast<GlobalVariable>(V);
  if (Global == nullptr or not FunctionTags::SegmentGlobal.isTagOf(Global))
    return nullptr;

  return Global;
}

bool SegmentAccessesToGEPPass::runOnFunction(llvm::Function &F) {
  llvm::SmallVector<IntToPtrInst *, 16> Candidates;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *ToPointer = dyn_cast<IntToPtrInst>(&I))
        Candidates.push_back(ToPointer);

  bool Changed = false;
  for (IntToPtrInst *ToPointer : Candidates) {
    auto *Add = dyn_cast<BinaryOperator>(skipIntegerCasts(ToPointer
                                                            ->getOperand(0)));
    if (Add == nullptr or Add->getOpcode() != Instruction::Add)
      continue;

    auto *Offset = dyn_cast<ConstantInt>(Add->getOperand(1));
    if (Offset == nullptr)
      continue;

    GlobalVariable *Segment = segmentGlobalOf(Add->getOperand(0));
    if (Segment == nullptr)
      continue;

    // A GEP past the end of the global would be undefined, and an offset that
    // large means this was never a reference into the segment.
    llvm::Type *Contents = Segment->getValueType();
    if (not Contents->isArrayTy()
        or Offset->getZExtValue() >= Contents->getArrayNumElements())
      continue;

    revng::IRBuilder Builder(ToPointer);
    Value *Access = Builder.CreateGEP(Builder.getInt8Ty(),
                                      Segment,
                                      Builder.getInt64(Offset
                                                         ->getZExtValue()));
    revng_log(Log,
              "Rewrote an access to " << Segment->getName().str() << " + "
                                      << Offset->getZExtValue());
    ToPointer->replaceAllUsesWith(Access);
    ToPointer->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

char SegmentAccessesToGEPPass::ID = 0;

static constexpr const char *Description = "Segment-access-to-i8-GEP "
                                           "replacement";

static llvm::RegisterPass<SegmentAccessesToGEPPass> X{ Flag,
                                                       Description,
                                                       false,
                                                       false };
