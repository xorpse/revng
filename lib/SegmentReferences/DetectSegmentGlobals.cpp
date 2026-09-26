//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "revng/Model/Binary.h"
#include "revng/Model/PrimitiveType.h"
#include "revng/SegmentReferences/DetectSegmentGlobals.h"
#include "revng/Support/Debug.h"
#include "revng/Support/IRHelpers.h"

using namespace llvm;

static Logger Log("detect-segment-globals");

/// The width of the access \p TheUse feeds, if it feeds one at all.
static std::optional<uint64_t> accessWidth(const Use &TheUse) {
  const User *User = TheUse.getUser();
  const Type *Accessed = nullptr;

  if (auto *Load = dyn_cast<LoadInst>(User)) {
    Accessed = Load->getType();
  } else if (auto *Store = dyn_cast<StoreInst>(User)) {
    // A store through the address, rather than a store *of* it somewhere else.
    if (TheUse.getOperandNo() != StoreInst::getPointerOperandIndex())
      return std::nullopt;
    Accessed = Store->getValueOperand()->getType();
  } else {
    return std::nullopt;
  }

  if (not Accessed->isIntegerTy() and not Accessed->isFloatingPointTy())
    return std::nullopt;

  uint64_t Bits = Accessed->getPrimitiveSizeInBits();
  if (Bits == 0 or Bits % 8 != 0)
    return std::nullopt;

  uint64_t Bytes = Bits / 8;
  if (not llvm::isPowerOf2_64(Bytes) or Bytes > 16)
    return std::nullopt;

  return Bytes;
}

void DetectSegmentGlobals::run(llvm::Module &M, llvm::Function *LimitTo) {
  for (auto &&SegmentUse : SegmentUses.getUses(M, LimitTo)) {
    std::optional<uint64_t> Width = accessWidth(*SegmentUse.TheUse);
    if (not Width.has_value()) {
      revng_log(Log,
                "Not a scalar access at " << SegmentUse.Address.toString()
                                          << ", ignoring");
      continue;
    }

    uint64_t &Recorded = Widths[SegmentUse.Address];
    Recorded = std::max(Recorded, *Width);
  }
}

void DetectSegmentGlobals::commit() {
  revng_log(Log, "Committing " << Widths.size() << " candidates");
  LoggerIndent Indent(Log);

  for (auto [Address, Width] : Widths) {
    const model::Segment *Segment = Binary.getSegmentFor(Address).first;
    if (Segment == nullptr)
      continue;

    // A read-only segment cannot change under the program, so its fields say
    // so too.
    auto Type = Segment->IsWriteable() ?
                  model::PrimitiveType::makeGeneric(Width) :
                  model::PrimitiveType::makeConstGeneric(Width);

    bool Success = GlobalBuilder.insert(Address, std::move(Type));
    revng_log(Log,
              (Success ? "Added " : "Not added ")
                << Width << " bytes at " << Address.toString());
  }

}

namespace revng::pypeline::analyses {

llvm::Error DetectSegmentGlobals::run(Model &Model,
                                      const Request &Incoming,
                                      llvm::StringRef Configuration,
                                      LLVMFunctionContainer &ModuleContainer) {
  ::DetectSegmentGlobals Detector(*Model.get().get());

  for (const ObjectID *Object : Incoming[0])
    Detector.run(ModuleContainer.getModule(*Object));

  Detector.commit();

  return llvm::Error::success();
}

} // namespace revng::pypeline::analyses
