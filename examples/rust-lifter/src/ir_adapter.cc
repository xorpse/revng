#include <cstdint>
#include <map>
#include <string>

#include "llvm-c/Types.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CBindingWrapping.h"

#include "revng/Model/FunctionTags.h"
#include "revng/Support/BasicBlockID.h"
#include "revng/Support/BlockType.h"

struct revng_rust_decoded_instruction {
  std::uint64_t Address;
  std::uint8_t Kind;
};

extern "C" const char *
revng_rust_emit_x86_64(std::uintptr_t OutputAddress,
                       const revng_rust_decoded_instruction *Instructions,
                       std::size_t InstructionCount, std::uint64_t Entry) {
  using namespace llvm;
  static thread_local std::string Error;
  auto Fail = [](const char *Message) -> const char * {
    Error = Message;
    return Error.c_str();
  };
  Error.clear();
  if (OutputAddress == 0)
    return Fail("null LLVM module");
  if (InstructionCount == 0)
    return Fail("no decoded instructions");

  Module &Output = *unwrap(reinterpret_cast<LLVMModuleRef>(OutputAddress));
  LLVMContext &Context = Output.getContext();
  IntegerType *I64 = Type::getInt64Ty(Context);
  IntegerType *I32 = Type::getInt32Ty(Context);
  PointerType *I8Ptr = Type::getInt8PtrTy(Context);
  auto MakeCSV = [&](StringRef Name) {
    auto *Global =
        new GlobalVariable(Output, I64, false, GlobalValue::ExternalLinkage,
                           ConstantInt::get(I64, 0), Name);
    FunctionTags::CSV.addTo(Global);
    return Global;
  };
  GlobalVariable *SP = MakeCSV("_rsp");
  GlobalVariable *PC = MakeCSV("_rip");
  auto MakePCComponent = [&](StringRef Name, unsigned Bits) {
    IntegerType *Ty = Type::getIntNTy(Context, Bits);
    return new GlobalVariable(Output, Ty, false, GlobalValue::ExternalLinkage,
                              ConstantInt::get(Ty, 0), Name);
  };
  GlobalVariable *PCEpoch = MakePCComponent("pc_epoch", 32);
  GlobalVariable *PCAddressSpace = MakePCComponent("pc_address_space", 16);
  GlobalVariable *PCType = MakePCComponent("pc_type", 16);

  FunctionType *RootType =
      FunctionType::get(Type::getVoidTy(Context), {I64}, false);
  Function *Root =
      Function::Create(RootType, Function::ExternalLinkage, "root", Output);
  FunctionTags::Root.addTo(Root);
  Root->addFnAttr(Attribute::NullPointerIsValid);

  FunctionType *NewPCType = FunctionType::get(
      Type::getVoidTy(Context), {I8Ptr, I64, I32, I32, I8Ptr}, true);
  Function *NewPC =
      Function::Create(NewPCType, Function::ExternalLinkage, "newpc", Output);
  FunctionTags::Marker.addTo(NewPC);
  NewPC->addFnAttr(Attribute::WillReturn);
  NewPC->addFnAttr(Attribute::NoUnwind);
  NewPC->addFnAttr(Attribute::NoMerge);

  BasicBlock *EntryBlock = BasicBlock::Create(Context, "entrypoint", Root);
  BasicBlock *Dispatcher = BasicBlock::Create(Context, "dispatcher", Root);
  BasicBlock *DispatcherDefault =
      BasicBlock::Create(Context, "dispatcher.default", Root);
  BasicBlock *AnyPC = BasicBlock::Create(Context, "anypc", Root);
  BasicBlock *UnexpectedPC = BasicBlock::Create(Context, "unexpectedpc", Root);

  IRBuilder<> Builder(EntryBlock);
  Builder.CreateStore(Root->getArg(0), SP);
  Builder.CreateStore(ConstantInt::get(I64, Entry), PC);
  Builder.CreateStore(ConstantInt::get(I32, 0), PCEpoch);
  Builder.CreateStore(ConstantInt::get(Type::getInt16Ty(Context), 0),
                      PCAddressSpace);
  Builder.CreateStore(
      ConstantInt::get(Type::getInt16Ty(Context), MetaAddressType::Code_x86_64),
      PCType);
  Builder.CreateBr(Dispatcher);

  Builder.SetInsertPoint(Dispatcher);
  Value *CurrentPC = Builder.CreateLoad(I64, PC);
  SwitchInst *Dispatch = Builder.CreateSwitch(CurrentPC, DispatcherDefault);

  std::map<std::uint64_t, BasicBlock *> Blocks;
  for (std::size_t I = 0; I < InstructionCount; ++I) {
    const auto &Instruction = Instructions[I];
    BasicBlock *Block = BasicBlock::Create(
        Context, "instruction_" + utohexstr(Instruction.Address), Root);
    Blocks.emplace(Instruction.Address, Block);
    Dispatch->addCase(ConstantInt::get(I64, Instruction.Address), Block);
  }

  for (std::size_t I = 0; I < InstructionCount; ++I) {
    const auto &Instruction = Instructions[I];
    Builder.SetInsertPoint(Blocks.at(Instruction.Address));
    MetaAddress Address =
        MetaAddress::fromPC(model::Architecture::x86_64, Instruction.Address);
    Value *Null = ConstantPointerNull::get(I8Ptr);
    Builder.CreateCall(NewPC,
                       {BasicBlockID(Address).toValue(&Output),
                        ConstantInt::get(I64, 1), ConstantInt::get(I32, I == 0),
                        ConstantInt::get(I32, 0), Null});
    if (Instruction.Kind == 0) {
      if (I + 1 == InstructionCount)
        return Fail("NOP is missing a successor");
      const std::uint64_t Next = Instructions[I + 1].Address;
      Builder.CreateStore(ConstantInt::get(I64, Next), PC);
      Builder.CreateBr(Blocks.at(Next));
    } else if (Instruction.Kind == 1) {
      BasicBlock *IndirectExit = BasicBlock::Create(
          Context, "indirect_exit_" + utohexstr(Instruction.Address), Root);
      Builder.CreateBr(IndirectExit);
      Builder.SetInsertPoint(IndirectExit);
      Value *StackPointer = Builder.CreateLoad(I64, SP);
      Value *StackPointerAsPointer =
          Builder.CreateIntToPtr(StackPointer, PointerType::get(Context, 0));
      Value *ReturnAddress = Builder.CreateLoad(I64, StackPointerAsPointer);
      Builder.CreateStore(ReturnAddress, PC);
      Builder.CreateStore(
          Builder.CreateAdd(StackPointer, ConstantInt::get(I64, 8)), SP);
      Builder.CreateBr(AnyPC);
    } else {
      return Fail("unknown decoded instruction kind");
    }
  }

  Builder.SetInsertPoint(AnyPC);
  setBlockType(Builder.CreateBr(Dispatcher), BlockType::AnyPCBlock);
  Builder.SetInsertPoint(UnexpectedPC);
  setBlockType(Builder.CreateBr(Dispatcher), BlockType::UnexpectedPCBlock);
  Builder.SetInsertPoint(DispatcherDefault);
  setBlockType(Builder.CreateUnreachable(), BlockType::DispatcherFailureBlock);
  setBlockType(Dispatch, BlockType::RootDispatcherBlock);
  return nullptr;
}
