//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE ReferenceLifter
#include <set>
#include <array>

#include "boost/test/unit_test.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include "revng/InlineHelpers/LinkHelpersToInline.h"
#include "revng/Lift/Lift.h"
#include "revng/Lift/PostLiftVerifyPass.h"
#include "revng/Lift/ReferenceLifter.h"
#include "revng/Loader/AddressSpaceLoader.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Support/NewPC.h"
#include "revng/UnitTestHelpers/UnitTestHelpers.h"

namespace {

class TestAddressSpace final : public revng::loader::AbstractAddressSpace {
private:
  std::array<uint8_t, 5> Bytes = { 0x89, 0xf8, 0x01, 0xf0, 0xc3 };
  std::array<revng::loader::Mapping, 1> Mappings;

public:
  TestAddressSpace() :
    Mappings({ revng::loader::Mapping{
      MetaAddress::fromPC(model::Architecture::x86_64, 0x400000),
      Bytes.size(),
      Bytes,
      true,
      false,
      true,
      "code" } }) {}

  model::Architecture::Values architecture() const override {
    return model::Architecture::x86_64;
  }
  std::optional<MetaAddress> entryPoint() const override {
    return Mappings[0].Start;
  }
  llvm::ArrayRef<revng::loader::Mapping> mappings() const override {
    return Mappings;
  }
};

} // namespace

BOOST_AUTO_TEST_CASE(EmitHelperFreeContractModule) {
  TestAddressSpace AddressSpace;
  auto Loaded = revng::loader::loadAddressSpace(AddressSpace);
  BOOST_REQUIRE(bool(Loaded));

  auto Lifter = revng::lift::LifterRegistry::create("reference-x86_64",
                                                    Loaded->Model);
  BOOST_REQUIRE(bool(Lifter));
  llvm::LLVMContext Context;
  llvm::Module Module("reference-lifter", Context);
  RawBinaryView View = Loaded->view();
  llvm::cantFail((*Lifter)->lift(*Loaded->Model, View, {}, Module));

  auto Verify = &llvm::verifyModule;
  const bool IsInvalid = Verify(Module, &llvm::errs(), nullptr);
  BOOST_CHECK(not IsInvalid);
  BOOST_CHECK(not PostLiftVerifyPass().runOnModule(Module));
  BOOST_CHECK(LinkHelpersToInlinePass().runOnModule(Module));
  llvm::Function *Root = Module.getFunction("root");
  BOOST_REQUIRE(Root != nullptr);
  BOOST_CHECK(FunctionTags::Root.isTagOf(Root));
  BOOST_CHECK(Root->size() >= 7U);
  BOOST_CHECK(Module.getGlobalVariable("_rsp") != nullptr);
  BOOST_CHECK(Module.getGlobalVariable("_rip") != nullptr);
  BOOST_CHECK(Module.getGlobalVariable("_rax") != nullptr);
  BOOST_CHECK(Module.getGlobalVariable("_rdi") != nullptr);
  BOOST_CHECK(Module.getGlobalVariable("_rsi") != nullptr);

  for (const llvm::Function &Function : Module)
    BOOST_CHECK(not Function.getName().starts_with("helper_"));

  // `NewPCArgument` names the arguments by position, and `FirstLocalVariable`
  // is both the last enumerator and the count of fixed arguments, since the
  // live locals follow it. Pin the marker's arity to it: the accessors index
  // positionally, so a marker with the wrong number of fixed arguments still
  // compiles, links and runs, and simply reads the wrong operand.
  llvm::Function *NewPC = Module.getFunction("newpc");
  BOOST_REQUIRE(NewPC != nullptr);
  BOOST_CHECK_EQUAL(NewPC->getFunctionType()->getNumParams(),
                    static_cast<unsigned>(NewPCArgument::FirstLocalVariable));
  BOOST_CHECK(NewPC->isVarArg());

  // Check the markers against the accessors the rest of the pipeline reads
  // them with.
  std::set<MetaAddress> Addresses;
  bool AnyStartsBasicBlock = false;
  for (const llvm::BasicBlock &BB : *Root) {
    for (const llvm::Instruction &I : BB) {
      std::optional Call = NewPCHelper.getCall(&I);
      if (not Call.has_value())
        continue;

      MetaAddress Address = addressFromNewPC(*Call);
      BOOST_CHECK(Address.isValid());
      Addresses.insert(Address);

      // Code belongs to a function only once it has been outlined, which has
      // not happened in `root`. If the argument list were misaligned this
      // would read a different operand and come back valid.
      BOOST_CHECK(ownerFromNewPC(*Call).isInvalid());

      AnyStartsBasicBlock = AnyStartsBasicBlock or startsBasicBlock(*Call);
    }
  }

  BOOST_CHECK(not Addresses.empty());
  BOOST_CHECK(AnyStartsBasicBlock);
  BOOST_CHECK(Addresses.contains(AddressSpace.entryPoint().value()));
}
