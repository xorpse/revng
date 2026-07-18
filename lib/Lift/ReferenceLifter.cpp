//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>

#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include "revng/Lift/ReferenceLifter.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Support/BasicBlockID.h"
#include "revng/Support/BlockType.h"
#include "revng/Support/Error.h"

namespace revng::lift {
namespace {

class ReferenceX86Lifter final : public ILifter {
public:
  llvm::Error lift(const model::Binary &Binary,
                   const RawBinaryView &View,
                   llvm::ArrayRef<MetaAddress> Entries,
                   llvm::Module &Output) override {
    using namespace llvm;
    if (Binary.Architecture() != model::Architecture::x86_64)
      return revng::createError("reference lifter only supports x86_64");

    SmallVector<MetaAddress, 1> Seeds(Entries);
    if (Seeds.empty() and Binary.EntryPoint().isValid())
      Seeds.push_back(Binary.EntryPoint());
    if (Seeds.empty())
      return revng::createError("reference lifter needs an entry address");

    LLVMContext &Context = Output.getContext();
    IntegerType *I64 = Type::getInt64Ty(Context);
    IntegerType *I32 = Type::getInt32Ty(Context);
    PointerType *I8Ptr = Type::getInt8PtrTy(Context);
    auto makeCSV = [&](StringRef Name) {
      auto *Global = new GlobalVariable(Output,
                                        I64,
                                        false,
                                        GlobalValue::ExternalLinkage,
                                        ConstantInt::get(I64, 0),
                                        Name);
      FunctionTags::CSV.addTo(Global);
      return Global;
    };
    GlobalVariable *SP = makeCSV("_rsp");
    GlobalVariable *PC = makeCSV("_rip");
    GlobalVariable *RAX = makeCSV("_rax");
    GlobalVariable *RDI = makeCSV("_rdi");
    GlobalVariable *RSI = makeCSV("_rsi");
    auto makePCComponent = [&](StringRef Name, size_t Size) {
      auto *ComponentType = Type::getIntNTy(Context, Size * 8);
      return new GlobalVariable(Output,
                                ComponentType,
                                false,
                                GlobalValue::ExternalLinkage,
                                ConstantInt::get(ComponentType, 0),
                                Name);
    };
    GlobalVariable *PCEpoch = makePCComponent("pc_epoch", sizeof(uint32_t));
    GlobalVariable *PCAddressSpace = makePCComponent("pc_address_space",
                                                     sizeof(uint16_t));
    GlobalVariable *PCType = makePCComponent("pc_type", sizeof(uint16_t));

    FunctionType *RootType = FunctionType::get(Type::getVoidTy(Context),
                                               { I64 },
                                               false);
    Function *Root = Function::Create(RootType,
                                      Function::ExternalLinkage,
                                      "root",
                                      Output);
    FunctionTags::Root.addTo(Root);
    Root->addFnAttr(Attribute::NullPointerIsValid);

    FunctionType *NewPCType = FunctionType::get(Type::getVoidTy(Context),
                                                { I8Ptr, I64, I32, I32, I8Ptr },
                                                true);
    Function *NewPC = Function::Create(NewPCType,
                                       Function::ExternalLinkage,
                                       "newpc",
                                       Output);
    FunctionTags::Marker.addTo(NewPC);
    NewPC->addFnAttr(Attribute::WillReturn);
    NewPC->addFnAttr(Attribute::NoUnwind);
    NewPC->addFnAttr(Attribute::NoMerge);

    BasicBlock *EntryBlock = BasicBlock::Create(Context, "entrypoint", Root);
    BasicBlock *Dispatcher = BasicBlock::Create(Context, "dispatcher", Root);
    BasicBlock *DispatcherDefault = BasicBlock::Create(Context,
                                                       "dispatcher.default",
                                                       Root);
    BasicBlock *AnyPC = BasicBlock::Create(Context, "anypc", Root);
    BasicBlock *UnexpectedPC = BasicBlock::Create(Context,
                                                  "unexpectedpc",
                                                  Root);

    llvm::IRBuilder<> Builder(EntryBlock);
    Builder.CreateStore(Root->getArg(0), SP);
    Builder.CreateStore(ConstantInt::get(I64, Seeds.front().address()), PC);
    Builder.CreateStore(ConstantInt::get(I32, Seeds.front().epoch()), PCEpoch);
    Builder.CreateStore(ConstantInt::get(Type::getInt16Ty(Context),
                                         Seeds.front().addressSpace()),
                        PCAddressSpace);
    Builder.CreateStore(ConstantInt::get(Type::getInt16Ty(Context),
                                         Seeds.front().type()),
                        PCType);
    Builder.CreateBr(Dispatcher);

    Builder.SetInsertPoint(Dispatcher);
    Value *CurrentPC = Builder.CreateLoad(I64, PC);
    SwitchInst *Dispatch = Builder.CreateSwitch(CurrentPC, DispatcherDefault);

    std::map<MetaAddress, BasicBlock *> InstructionBlocks;
    for (MetaAddress Seed : Seeds) {
      MetaAddress Address = Seed;
      BasicBlock *Block = nullptr;
      for (unsigned InstructionCount = 0; InstructionCount != 64;
           ++InstructionCount) {
        if (Block == nullptr) {
          auto [Iterator, Inserted] = InstructionBlocks.emplace(Address,
                                                                nullptr);
          if (not Inserted)
            break;
          Block = BasicBlock::Create(Context,
                                     "instruction_"
                                       + utohexstr(Address.address()),
                                     Root);
          Iterator->second = Block;
          Dispatch->addCase(ConstantInt::get(I64, Address.address()), Block);
        }

        auto Byte = View.getByAddress(Address, 1);
        if (not Byte)
          return revng::createError("reference lifter cannot read instruction");
        const uint8_t Opcode = (*Byte)[0];
        uint64_t InstructionSize = 1;
        enum class Semantics { Nop, Ret, MovEAXEDI, AddEAXESI };
        Semantics InstructionSemantics;
        if (Opcode == 0x90) {
          InstructionSemantics = Semantics::Nop;
        } else if (Opcode == 0xc3) {
          InstructionSemantics = Semantics::Ret;
        } else if (Opcode == 0x89 or Opcode == 0x01) {
          auto Bytes = View.getByAddress(Address, 2);
          if (not Bytes)
            return revng::createError("reference lifter cannot read instruction");
          if (Opcode == 0x89 and (*Bytes)[1] == 0xf8)
            InstructionSemantics = Semantics::MovEAXEDI;
          else if (Opcode == 0x01 and (*Bytes)[1] == 0xf0)
            InstructionSemantics = Semantics::AddEAXESI;
          else
            return revng::createError("reference lifter encountered unsupported "
                                      "opcode");
          InstructionSize = 2;
        } else {
          return revng::createError("reference lifter encountered unsupported "
                                    "opcode");
        }

        Builder.SetInsertPoint(Block);
        Value *Null = ConstantPointerNull::get(I8Ptr);
        Builder.CreateCall(NewPC,
                           { BasicBlockID(Address).toValue(&Output),
                             ConstantInt::get(I64, InstructionSize),
                             ConstantInt::get(I32, InstructionCount == 0),
                             ConstantInt::get(I32, 0),
                             Null });
        if (InstructionSemantics != Semantics::Ret) {
          if (InstructionSemantics == Semantics::MovEAXEDI) {
            Value *Value = Builder.CreateLoad(I64, RDI);
            Value = Builder.CreateTrunc(Value, I32);
            Builder.CreateStore(Builder.CreateZExt(Value, I64), RAX);
          } else if (InstructionSemantics == Semantics::AddEAXESI) {
            Value *Left = Builder.CreateTrunc(Builder.CreateLoad(I64, RAX),
                                              I32);
            Value *Right = Builder.CreateTrunc(Builder.CreateLoad(I64, RSI),
                                               I32);
            Value *Result = Builder.CreateAdd(Left, Right);
            Builder.CreateStore(Builder.CreateZExt(Result, I64), RAX);
          }
          MetaAddress Next = Address + InstructionSize;
          Builder.CreateStore(ConstantInt::get(I64, Next.address()), PC);
          auto Existing = InstructionBlocks.find(Next);
          if (Existing != InstructionBlocks.end()) {
            Builder.CreateBr(Existing->second);
            break;
          }
          BasicBlock
            *NextBlock = BasicBlock::Create(Context,
                                            "instruction_"
                                              + utohexstr(Next.address()),
                                            Root);
          InstructionBlocks.emplace(Next, NextBlock);
          Dispatch->addCase(ConstantInt::get(I64, Next.address()), NextBlock);
          Builder.CreateBr(NextBlock);
          Address = Next;
          Block = NextBlock;
        } else {
          BasicBlock
            *IndirectExit = BasicBlock::Create(Context,
                                               "indirect_exit_"
                                                 + utohexstr(Address.address()),
                                               Root);
          Builder.CreateBr(IndirectExit);
          Builder.SetInsertPoint(IndirectExit);
          Value *StackPointer = Builder.CreateLoad(I64, SP);
          Value
            *StackPointerAsPointer = Builder
                                       .CreateIntToPtr(StackPointer,
                                                       PointerType::get(Context,
                                                                        0));
          Value *ReturnAddress = Builder.CreateLoad(I64, StackPointerAsPointer);
          Builder.CreateStore(ReturnAddress, PC);
          Builder.CreateStore(Builder.CreateAdd(StackPointer,
                                                ConstantInt::get(I64, 8)),
                              SP);
          Builder.CreateBr(AnyPC);
          break;
        }
      }
    }

    Builder.SetInsertPoint(AnyPC);
    setBlockType(Builder.CreateBr(Dispatcher), BlockType::AnyPCBlock);
    Builder.SetInsertPoint(UnexpectedPC);
    setBlockType(Builder.CreateBr(Dispatcher), BlockType::UnexpectedPCBlock);
    Builder.SetInsertPoint(DispatcherDefault);
    setBlockType(Builder.CreateUnreachable(),
                 BlockType::DispatcherFailureBlock);
    setBlockType(Dispatch, BlockType::RootDispatcherBlock);
    return llvm::Error::success();
  }
};

} // namespace

std::unique_ptr<ILifter> createReferenceX86Lifter() {
  return std::make_unique<ReferenceX86Lifter>();
}

namespace {
bool Registered = []() {
  LifterFactory Factory = [](const TupleTree<model::Binary> &) {
    return createReferenceX86Lifter();
  };
  llvm::cantFail(LifterRegistry::registerLifter("reference-x86_64",
                                                std::move(Factory),
                                                false,
                                                { model::Architecture::x86_64 }));
  return true;
}();
} // namespace

} // namespace revng::lift
