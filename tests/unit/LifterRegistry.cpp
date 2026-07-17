//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE LifterRegistry
#include <array>

#include "boost/test/unit_test.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "revng/Lift/AbstractLifter.h"
#include "revng/UnitTestHelpers/UnitTestHelpers.h"

namespace {

bool WasCalled = false;

class TestLifter final : public revng::lift::ILifter {
public:
  llvm::Error lift(const model::Binary &,
                   const RawBinaryView &,
                   llvm::ArrayRef<MetaAddress>,
                   llvm::Module &) override {
    WasCalled = true;
    return llvm::Error::success();
  }
};

} // namespace

BOOST_AUTO_TEST_CASE(RegisterCreateAndInvoke) {
  using namespace revng::lift;
  LifterFactory Factory = [](const TupleTree<model::Binary> &) {
    return std::make_unique<TestLifter>();
  };
  llvm::cantFail(LifterRegistry::registerLifter("unit-test-lifter",
                                                std::move(Factory)));

  TupleTree<model::Binary> Model;
  Model->Architecture() = model::Architecture::x86_64;
  std::array<uint8_t, 1> Byte = { 0 };
  RawBinaryView View(*Model, Byte);
  llvm::LLVMContext Context;
  llvm::Module Module("test", Context);

  auto Lifter = LifterRegistry::create("unit-test-lifter", Model);
  BOOST_REQUIRE(bool(Lifter));
  llvm::cantFail((*Lifter)->lift(*Model, View, {}, Module));
  BOOST_CHECK(WasCalled);

  LifterFactory DuplicateFactory = [](const TupleTree<model::Binary> &) {
    return std::make_unique<TestLifter>();
  };
  auto Duplicate = LifterRegistry::registerLifter("unit-test-lifter",
                                                  std::move(DuplicateFactory));
  BOOST_CHECK(bool(Duplicate));
  llvm::consumeError(std::move(Duplicate));

  auto Missing = LifterRegistry::create("missing-unit-test-lifter", Model);
  BOOST_CHECK(not bool(Missing));
  llvm::consumeError(Missing.takeError());
}

#ifdef REVNG_TEST_NO_LIBTCG
BOOST_AUTO_TEST_CASE(NoImplicitDefaultBackend) {
  TupleTree<model::Binary> Model;
  auto Lifter = revng::lift::LifterRegistry::createDefault(Model);
  BOOST_CHECK(not bool(Lifter));
  llvm::consumeError(Lifter.takeError());
}
#endif
