//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE PipelineCAddressSpace
#include <algorithm>
#include <array>
#include <string_view>

#include "boost/test/unit_test.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/Lift/AbstractLifter.h"
#include "revng/Lift/ReferenceLifter.h"
#include "revng/Model/RawBinaryView.h"
#include "revng/PipelineC/PipelineC.h"
#include "revng/Pipes/ModelGlobal.h"
#include "revng/UnitTestHelpers/UnitTestHelpers.h"

#include "llvm-c/Core.h"

namespace {

struct HostState {
  std::array<uint8_t, 2> Bytes = {0x90, 0xc3};
  std::string Start =
      MetaAddress::fromPC(model::Architecture::x86_64, 0x400000).toString();
  bool LifterCalled = false;
  uint64_t ReadCalls = 0;
  uint64_t ReleaseCalls = 0;
};

const char *architecture(void *) { return "x86_64"; }
const char *entryPoint(void *Opaque) {
  return static_cast<HostState *>(Opaque)->Start.c_str();
}
uint64_t mappingCount(void *) { return 1; }
uint64_t noExtraCodeAddresses(void *) { return 0; }

bool mappingAt(void *Opaque, uint64_t Index, rp_address_space_mapping *Output) {
  if (Index != 0)
    return false;
  auto &State = *static_cast<HostState *>(Opaque);
  *Output = {State.Start.c_str(), 4, State.Bytes.size(), true, false, true,
             "lazy-code"};
  return true;
}

bool readBytes(void *Opaque, uint64_t MappingIndex, uint64_t Offset,
               uint8_t *Destination, uint64_t Size) {
  auto &State = *static_cast<HostState *>(Opaque);
  if (MappingIndex != 0 or Offset > State.Bytes.size() or
      Size > State.Bytes.size() - Offset)
    return false;
  ++State.ReadCalls;
  std::copy_n(State.Bytes.data() + Offset, Size, Destination);
  return true;
}

void releaseAddressSpace(void *Opaque) {
  ++static_cast<HostState *>(Opaque)->ReleaseCalls;
}

bool lift(void *Opaque, const char *Model, const rp_binary_view *Binary,
          const char *const *, uint64_t, LLVMModuleRef Output, const char **) {
  auto &State = *static_cast<HostState *>(Opaque);
  uint8_t Opcode = 0;
  rp_error Error;
  State.LifterCalled = Model != nullptr and rp_binary_view_size(Binary) == 4 and
                       rp_binary_view_read_address(Binary, State.Start.c_str(),
                                                   1, &Opcode, &Error) and
                       Opcode == 0x90;
  LLVMContextRef Context = LLVMGetModuleContext(Output);
  LLVMTypeRef Type =
      LLVMFunctionType(LLVMVoidTypeInContext(Context), nullptr, 0, false);
  LLVMAddFunction(Output, "host_lifter_was_here", Type);
  return State.LifterCalled;
}

} // namespace

BOOST_AUTO_TEST_CASE(CreateManagerAndSetLifter) {
  const std::string PipelineOption =
      "--pipeline-path=" + std::string(REVNG_SOURCE_DIR) +
      "/tests/unit/PipelineCAddressSpace.yml";
  const char *Arguments[] = {"test-pipeline-c-address-space",
                             PipelineOption.c_str(),
                             "--lifter-backend=reference-x86_64"};
  BOOST_REQUIRE(rp_initialize(3, Arguments, 0, nullptr));

  auto ReferenceBackend = revng::lift::createReferenceX86Lifter();
  BOOST_REQUIRE(ReferenceBackend != nullptr);

  HostState State;
  rp_address_space_callbacks AddressSpace{&State,
                                          architecture,
                                          entryPoint,
                                          mappingCount,
                                          mappingAt,
                                          readBytes,
                                          noExtraCodeAddresses,
                                          nullptr,
                                          releaseAddressSpace};
  rp_error Error;
  rp_manager *Manager = rp_manager_create_from_address_space(
      &AddressSpace, 1, 0, nullptr, "", &Error);
  if (Manager == nullptr) {
    if (auto *Simple = std::get_if<rp_simple_error>(&Error))
      llvm::errs() << "manager creation failed: " << Simple->Message << "\n";
  }
  BOOST_REQUIRE(Manager != nullptr);

  const auto &Model = revng::getModelFromContext(Manager->context());
  BOOST_CHECK(Model->Architecture() == model::Architecture::x86_64);
  BOOST_CHECK(Model->Segments().size() == 1U);
  BOOST_CHECK(Model->DefaultABI() == model::ABI::SystemV_x86_64);
  BOOST_REQUIRE(rp_manager_set_target_abi(Manager, "SystemV_x86_64", &Error));
  BOOST_REQUIRE(rp_manager_set_default_abi(Manager, "SystemV_x86_64", &Error));
  BOOST_REQUIRE(rp_manager_set_operating_system(Manager, "Linux", &Error));
  BOOST_REQUIRE(
      rp_manager_set_platform_name(Manager, "embedded-linux-x86_64", &Error));
  const std::string ExtraAddress =
      MetaAddress::fromPC(model::Architecture::x86_64, 0x400003).toString();
  BOOST_REQUIRE(
      rp_manager_add_extra_code_address(Manager, ExtraAddress.c_str(), &Error));
  BOOST_CHECK(Model->TargetABI() == model::ABI::SystemV_x86_64);
  BOOST_CHECK(
      Model->Functions().at(MetaAddress::fromString(State.Start)).Prototype() ==
      Model->DefaultPrototype());
  BOOST_CHECK(Model->OperatingSystem() == model::OperatingSystem::Linux);
  BOOST_CHECK(Model->PlatformName() == "embedded-linux-x86_64");

  const rp_primitive_type I32{RP_PRIMITIVE_KIND_SIGNED, 4};
  const rp_cabi_argument AddArguments[] = {{"a", I32}, {"b", I32}};
  BOOST_REQUIRE(rp_manager_set_cabi_prototype(Manager, State.Start.c_str(),
                                              "SystemV_x86_64", "add", 2,
                                              AddArguments, &I32, &Error));
  const model::Function &EntryFunction =
      Model->Functions().at(MetaAddress::fromString(State.Start));
  BOOST_CHECK(EntryFunction.Name() == "add");
  auto *Prototype = EntryFunction.Prototype()->getCABIFunction();
  BOOST_REQUIRE(Prototype != nullptr);
  BOOST_CHECK(Prototype->ABI() == model::ABI::SystemV_x86_64);
  BOOST_CHECK(Prototype->Arguments().size() == 2U);

  rp_step *LiftStep = rp_manager_get_step_from_name(Manager, "lift");
  const rp_container_identifier *RootIdentifier =
      rp_manager_get_container_identifier_from_name(Manager, "root.bc."
                                                             "zstd");
  rp_container *RootContainer = rp_step_get_container(LiftStep, RootIdentifier);
  const rp_kind *RootKind = rp_manager_get_kind_from_name(Manager, "root");
  const char *NoPath[] = {nullptr};
  rp_target *RootTarget = rp_target_create(RootKind, 0, NoPath);
  const rp_target *Targets[] = {RootTarget};
  rp_buffer *LiftedModule = rp_manager_produce_targets(
      Manager, LiftStep, RootContainer, 1, Targets, &Error);
  if (LiftedModule == nullptr) {
    if (auto *Simple = std::get_if<rp_simple_error>(&Error))
      llvm::errs() << "lifting failed: " << Simple->Message << "\n";
  }
  BOOST_REQUIRE(LiftedModule != nullptr);
  BOOST_CHECK(rp_buffer_size(LiftedModule) != 0U);
  rp_buffer_destroy(LiftedModule);
  rp_target_destroy(RootTarget);

  rp_lifter_callbacks LifterCallbacks{&State, lift};
  BOOST_REQUIRE(rp_set_lifter(Manager, &LifterCallbacks, &Error));

  auto Lifter = revng::lift::LifterRegistry::createDefault(Model);
  BOOST_REQUIRE(bool(Lifter));
  std::array<uint8_t, 4> Bytes = {0x90, 0xc3, 0, 0};
  RawBinaryView View(*Model, Bytes);
  llvm::LLVMContext Context;
  llvm::Module Module("host-lifter-test", Context);
  llvm::cantFail((*Lifter)->lift(*Model, View, {}, Module));
  BOOST_CHECK(State.LifterCalled);
  BOOST_CHECK(Module.getFunction("host_lifter_was_here") != nullptr);

  BOOST_CHECK(rp_lifter_backend_count() >= 1U);
  bool FoundReference = false;
  for (uint64_t I = 0; I < rp_lifter_backend_count(); ++I) {
    char *Name = rp_lifter_backend_name(I);
    BOOST_REQUIRE(Name != nullptr);
    if (std::string_view(Name) == "reference-x86_64")
      FoundReference = true;
    rp_string_destroy(Name);
  }
  BOOST_CHECK(FoundReference);
  BOOST_CHECK(
      rp_lifter_backend_supports_architecture("reference-x86_64", "x86_64"));
  BOOST_CHECK(not rp_lifter_backend_supports_architecture("reference-x86_64",
                                                          "aarch64"));
  BOOST_CHECK(rp_architecture_count() >= 7U);
  BOOST_CHECK(rp_abi_count("x86_64") >= 2U);

  const rp_type SignedI32{RP_TYPE_KIND_PRIMITIVE, false, I32, 0, 0, nullptr};
  const uint64_t PairID = rp_manager_create_struct_type(
      Manager, "pair", "two signed values", 8, false, &Error);
  BOOST_REQUIRE(PairID != uint64_t(-1));
  BOOST_REQUIRE(rp_manager_add_struct_field(Manager, PairID, 0, "first",
                                            "first value", &SignedI32, &Error));
  BOOST_REQUIRE(rp_manager_add_struct_field(
      Manager, PairID, 4, "second", "second value", &SignedI32, &Error));
  const rp_type Pair{RP_TYPE_KIND_DEFINED, false, {}, 0, PairID, nullptr};
  const rp_type PairPointer{RP_TYPE_KIND_POINTER, false, {}, 0, 0, &Pair};
  const rp_type PairArray{RP_TYPE_KIND_ARRAY, false, {}, 2, 0, &Pair};
  const uint64_t PairArrayID = rp_manager_create_typedef(
      Manager, "pair_array", "two pairs", &PairArray, &Error);
  BOOST_REQUIRE(PairArrayID != uint64_t(-1));
  const uint64_t ValueID =
      rp_manager_create_union_type(Manager, "value", "number or pair", &Error);
  BOOST_REQUIRE(rp_manager_add_union_field(
      Manager, ValueID, "number", "numeric alternative", &SignedI32, &Error));
  BOOST_REQUIRE(rp_manager_add_union_field(Manager, ValueID, "pair",
                                           "pair alternative", &Pair, &Error));
  const rp_primitive_type U32{RP_PRIMITIVE_KIND_UNSIGNED, 4};
  const uint64_t KindID = rp_manager_create_enum_type(
      Manager, "kind", "value discriminator", &U32, &Error);
  BOOST_REQUIRE(KindID != uint64_t(-1));
  BOOST_REQUIRE(rp_manager_add_enum_entry(Manager, KindID, 1, "pair_kind",
                                          "the pair alternative", &Error));
  const rp_typed_argument ProcessArguments[] = {
      {"value", "value to process", PairPointer}};
  const uint64_t ProcessType = rp_manager_create_cabi_type(
      Manager, "process_type", "processes a pair", "SystemV_x86_64", 1,
      ProcessArguments, &SignedI32, "processing result", &Error);
  BOOST_REQUIRE(ProcessType != uint64_t(-1));

  const rp_named_typed_register RawArguments[] = {
      {"rdi", "first", "first register argument", SignedI32},
      {"rsi", "second", "second register argument", SignedI32},
  };
  const rp_named_typed_register RawReturns[] = {
      {"rax", "result", "register result", SignedI32},
  };
  const char *PreservedRegisters[] = {"rbx"};
  const uint64_t RawType = rp_manager_create_raw_function_type(
      Manager, "raw_add_type", "raw register-level prototype", "x86_64", 2,
      RawArguments, 1, RawReturns, 1, PreservedRegisters, 8, &Pair,
      "register return value", &Error);
  BOOST_REQUIRE(RawType != uint64_t(-1));
  const std::string ProcessAddress =
      MetaAddress::fromPC(model::Architecture::x86_64, 0x400002).toString();
  BOOST_REQUIRE(rp_manager_add_function(Manager, ProcessAddress.c_str(),
                                        "process", &Error));
  BOOST_REQUIRE(rp_manager_set_function_prototype(
      Manager, ProcessAddress.c_str(), ProcessType, &Error));
  BOOST_REQUIRE(rp_manager_add_function_exported_name(
      Manager, ProcessAddress.c_str(), "process_export", &Error));
  BOOST_REQUIRE(rp_manager_add_imported_library(Manager, "libhost.so", &Error));
  BOOST_REQUIRE(rp_manager_add_imported_function(Manager, "host_process",
                                                 ProcessType, &Error));
  BOOST_REQUIRE(rp_manager_add_data_symbol(Manager, State.Start.c_str(),
                                           "global_value", &SignedI32, &Error));
  BOOST_CHECK(Model->Functions().size() == 2U);
  BOOST_CHECK(Model->ImportedLibraries().contains("libhost.so"));
  BOOST_CHECK(Model->ImportedDynamicFunctions().contains("host_process"));
  BOOST_CHECK(Model->TypeDefinitions().size() >= 6U);

  rp_manager_destroy(Manager);

  State.LifterCalled = false;
  State.ReadCalls = 0;
  State.ReleaseCalls = 0;
  Manager = rp_manager_create_from_address_space(&AddressSpace, 0, 0, nullptr,
                                                 "", &Error);
  BOOST_REQUIRE(Manager != nullptr);
  BOOST_CHECK(State.ReadCalls == 0U);
  BOOST_CHECK(State.ReleaseCalls == 0U);
  BOOST_CHECK(not rp_manager_save(Manager));

  rp_lifter_callbacks LazyLifterCallbacks{&State, lift};
  BOOST_REQUIRE(rp_set_lifter(Manager, &LazyLifterCallbacks, &Error));
  const auto &LazyModel = revng::getModelFromContext(Manager->context());
  {
    auto LazyLifter = revng::lift::LifterRegistry::createDefault(LazyModel);
    BOOST_REQUIRE(bool(LazyLifter));
    std::array<uint8_t, 1> Placeholder = {0};
    RawBinaryView LazyView(*LazyModel, Placeholder);
    llvm::Module LazyModule("lazy-host-lifter-test", Context);
    llvm::cantFail((*LazyLifter)->lift(*LazyModel, LazyView, {}, LazyModule));
  }
  BOOST_CHECK(State.LifterCalled);
  BOOST_CHECK(State.ReadCalls == 1U);
  BOOST_CHECK(State.ReleaseCalls == 0U);

  BOOST_REQUIRE(rp_manager_materialize_address_space(Manager, &Error));
  BOOST_CHECK(State.ReadCalls == 2U);
  BOOST_CHECK(State.ReleaseCalls == 1U);
  BOOST_CHECK(rp_manager_save(Manager));
  rp_manager_destroy(Manager);
  BOOST_CHECK(State.ReleaseCalls == 1U);

  State.ReadCalls = 0;
  State.ReleaseCalls = 0;
  Manager = rp_manager_create_from_address_space(&AddressSpace, 1, 0, nullptr,
                                                 "", &Error);
  BOOST_REQUIRE(Manager != nullptr);
  BOOST_CHECK(State.ReadCalls == 1U);
  BOOST_CHECK(State.ReleaseCalls == 1U);
  BOOST_CHECK(rp_manager_save(Manager));
  rp_manager_destroy(Manager);
  BOOST_CHECK(State.ReleaseCalls == 1U);

  int FileDescriptor = -1;
  llvm::SmallString<128> FilePath;
  BOOST_REQUIRE(not llvm::sys::fs::createTemporaryFile(
      "revng-file-provider", "bin", FileDescriptor, FilePath));
  {
    llvm::raw_fd_ostream Stream(FileDescriptor, true);
    Stream.write(reinterpret_cast<const char *>(State.Bytes.data()),
                 State.Bytes.size());
  }
  rp_file_address_space_mapping FileMapping{State.Start.c_str(),
                                            4,
                                            State.Bytes.size(),
                                            FilePath.c_str(),
                                            0,
                                            true,
                                            false,
                                            true,
                                            "file-code"};
  Manager = rp_manager_create_from_file_address_space(
      "x86_64", State.Start.c_str(), 1, &FileMapping, 0, 0, nullptr, "",
      &Error);
  BOOST_REQUIRE(Manager != nullptr);
  const auto &FileModel = revng::getModelFromContext(Manager->context());
  const std::array<uint8_t, 1> FilePlaceholder = {0};
  RawBinaryView FileView(*FileModel, FilePlaceholder);
  auto FileBytes = FileView.getByAddress(MetaAddress::fromString(State.Start),
                                         State.Bytes.size());
  BOOST_REQUIRE(FileBytes.has_value());
  BOOST_CHECK(
      std::equal(FileBytes->begin(), FileBytes->end(), State.Bytes.begin()));
  rp_manager_destroy(Manager);
  BOOST_CHECK(not llvm::sys::fs::remove(FilePath));
  BOOST_CHECK(rp_shutdown());
}
