//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE PipelineCAddressSpace
#include <array>

#include "boost/test/unit_test.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "revng/Lift/AbstractLifter.h"
#include "revng/Lift/ReferenceLifter.h"
#include "revng/Model/RawBinaryView.h"
#include "revng/PipelineC/PipelineC.h"
#include "revng/Pipes/ModelGlobal.h"
#include "revng/UnitTestHelpers/UnitTestHelpers.h"

#include "llvm-c/Core.h"

namespace {

struct HostState {
  std::array<uint8_t, 2> Bytes = { 0x90, 0xc3 };
  std::string Start = MetaAddress::fromPC(model::Architecture::x86_64, 0x400000)
                        .toString();
  bool LifterCalled = false;
};

const char *architecture(void *) {
  return "x86_64";
}
const char *entryPoint(void *Opaque) {
  return static_cast<HostState *>(Opaque)->Start.c_str();
}
uint64_t mappingCount(void *) {
  return 1;
}
bool mappingAt(void *Opaque, uint64_t Index, rp_address_space_mapping *Output) {
  if (Index != 0)
    return false;
  auto &State = *static_cast<HostState *>(Opaque);
  *Output = { State.Start.c_str(),
              4,
              State.Bytes.data(),
              State.Bytes.size(),
              true,
              false,
              true,
              "code" };
  return true;
}
uint64_t noExtraCodeAddresses(void *) {
  return 0;
}

bool lift(void *Opaque,
          const char *Model,
          const uint8_t *Binary,
          uint64_t BinarySize,
          const char *const *,
          uint64_t,
          LLVMModuleRef Output,
          const char **) {
  auto &State = *static_cast<HostState *>(Opaque);
  State.LifterCalled = Model != nullptr and Binary != nullptr
                       and BinarySize == 4 and Binary[0] == 0x90
                       and Binary[2] == 0;
  LLVMContextRef Context = LLVMGetModuleContext(Output);
  LLVMTypeRef Type = LLVMFunctionType(LLVMVoidTypeInContext(Context),
                                      nullptr,
                                      0,
                                      false);
  LLVMAddFunction(Output, "host_lifter_was_here", Type);
  return true;
}

} // namespace

BOOST_AUTO_TEST_CASE(CreateManagerAndSetLifter) {
  const std::string PipelineOption = "--pipeline-path="
                                     + std::string(REVNG_SOURCE_DIR)
                                     + "/tests/unit/PipelineCAddressSpace.yml";
  const char *Arguments[] = { "test-pipeline-c-address-space",
                              PipelineOption.c_str(),
                              "--lifter-backend=reference-x86_64" };
  BOOST_REQUIRE(rp_initialize(3, Arguments, 0, nullptr));

  auto ReferenceBackend = revng::lift::createReferenceX86Lifter();
  BOOST_REQUIRE(ReferenceBackend != nullptr);

  HostState State;
  rp_address_space_callbacks AddressSpace{ &State,     architecture,
                                           entryPoint, mappingCount,
                                           mappingAt,  noExtraCodeAddresses,
                                           nullptr };
  rp_error Error;
  rp_manager *Manager = rp_manager_create_from_address_space(&AddressSpace,
                                                             0,
                                                             nullptr,
                                                             "",
                                                             &Error);
  if (Manager == nullptr) {
    if (auto *Simple = std::get_if<rp_simple_error>(&Error))
      llvm::errs() << "manager creation failed: " << Simple->Message << "\n";
  }
  BOOST_REQUIRE(Manager != nullptr);

  const auto &Model = revng::getModelFromContext(Manager->context());
  BOOST_CHECK(Model->Architecture() == model::Architecture::x86_64);
  BOOST_CHECK(Model->Segments().size() == 1U);
  BOOST_CHECK(Model->DefaultABI() == model::ABI::SystemV_x86_64);

  rp_step *LiftStep = rp_manager_get_step_from_name(Manager, "lift");
  const rp_container_identifier
    *RootIdentifier = rp_manager_get_container_identifier_from_name(Manager,
                                                                    "root.bc."
                                                                    "zstd");
  rp_container *RootContainer = rp_step_get_container(LiftStep, RootIdentifier);
  const rp_kind *RootKind = rp_manager_get_kind_from_name(Manager, "root");
  const char *NoPath[] = { nullptr };
  rp_target *RootTarget = rp_target_create(RootKind, 0, NoPath);
  const rp_target *Targets[] = { RootTarget };
  rp_buffer *LiftedModule = rp_manager_produce_targets(Manager,
                                                       LiftStep,
                                                       RootContainer,
                                                       1,
                                                       Targets,
                                                       &Error);
  if (LiftedModule == nullptr) {
    if (auto *Simple = std::get_if<rp_simple_error>(&Error))
      llvm::errs() << "lifting failed: " << Simple->Message << "\n";
  }
  BOOST_REQUIRE(LiftedModule != nullptr);
  BOOST_CHECK(rp_buffer_size(LiftedModule) != 0U);
  rp_buffer_destroy(LiftedModule);
  rp_target_destroy(RootTarget);

  rp_lifter_callbacks LifterCallbacks{ &State, lift };
  BOOST_REQUIRE(rp_set_lifter(Manager, &LifterCallbacks, &Error));

  auto Lifter = revng::lift::LifterRegistry::createDefault(Model);
  BOOST_REQUIRE(bool(Lifter));
  std::array<uint8_t, 4> Bytes = { 0x90, 0xc3, 0, 0 };
  RawBinaryView View(*Model, Bytes);
  llvm::LLVMContext Context;
  llvm::Module Module("host-lifter-test", Context);
  llvm::cantFail((*Lifter)->lift(*Model, View, {}, Module));
  BOOST_CHECK(State.LifterCalled);
  BOOST_CHECK(Module.getFunction("host_lifter_was_here") != nullptr);

  rp_manager_destroy(Manager);
  BOOST_CHECK(rp_shutdown());
}
