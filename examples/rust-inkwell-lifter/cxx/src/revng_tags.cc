#include <cstdint>

#include "llvm-c/Types.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/CBindingWrapping.h"

#include "revng/Model/FunctionTags.h"
#include "revng/Support/BasicBlockID.h"
#include "revng/Support/BlockType.h"

namespace revng_inkwell {

void tag_root(std::uintptr_t Value) {
  auto *Function = llvm::cast<llvm::Function>(
      llvm::unwrap(reinterpret_cast<LLVMValueRef>(Value)));
  FunctionTags::Root.addTo(Function);
  Function->addFnAttr(llvm::Attribute::NullPointerIsValid);
}

void tag_marker(std::uintptr_t Value) {
  auto *Function = llvm::cast<llvm::Function>(
      llvm::unwrap(reinterpret_cast<LLVMValueRef>(Value)));
  FunctionTags::Marker.addTo(Function);
  Function->addFnAttr(llvm::Attribute::WillReturn);
  Function->addFnAttr(llvm::Attribute::NoUnwind);
  Function->addFnAttr(llvm::Attribute::NoMerge);
}

void tag_csv(std::uintptr_t Value) {
  auto *Global = llvm::cast<llvm::GlobalVariable>(
      llvm::unwrap(reinterpret_cast<LLVMValueRef>(Value)));
  FunctionTags::CSV.addTo(Global);
}

std::uintptr_t basic_block_id(std::uintptr_t Value, std::uint64_t Address) {
  llvm::Module *Module =
      llvm::unwrap(reinterpret_cast<LLVMModuleRef>(Value));
  MetaAddress PC =
      MetaAddress::fromPC(model::Architecture::x86_64, Address);
  return reinterpret_cast<std::uintptr_t>(
      llvm::wrap(BasicBlockID(PC).toValue(Module)));
}

std::uint16_t code_x86_64_type() {
  return static_cast<std::uint16_t>(MetaAddressType::Code_x86_64);
}

void set_block_type(std::uintptr_t Value, std::uint8_t Kind) {
  auto *Terminator = llvm::cast<llvm::Instruction>(
      llvm::unwrap(reinterpret_cast<LLVMValueRef>(Value)));
  switch (Kind) {
  case 0:
    setBlockType(Terminator, BlockType::RootDispatcherBlock);
    return;
  case 1:
    setBlockType(Terminator, BlockType::DispatcherFailureBlock);
    return;
  case 2:
    setBlockType(Terminator, BlockType::AnyPCBlock);
    return;
  case 3:
    setBlockType(Terminator, BlockType::UnexpectedPCBlock);
    return;
  default:
    revng_abort("invalid block type");
  }
}

} // namespace revng_inkwell
