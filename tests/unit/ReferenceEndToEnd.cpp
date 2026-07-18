//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE ReferenceEndToEnd
#include <array>

#include "boost/test/unit_test.hpp"

#include "revng/PipelineC/PipelineC.h"
#include "revng/UnitTestHelpers/UnitTestHelpers.h"

namespace {

struct AddressSpace {
  std::array<uint8_t, 5> Bytes = { 0x89, 0xf8, 0x01, 0xf0, 0xc3 };
  std::string Entry = "0x400000:Code_x86_64";
};

const char *architecture(void *) {
  return "x86_64";
}
const char *entryPoint(void *Opaque) {
  return static_cast<AddressSpace *>(Opaque)->Entry.c_str();
}
uint64_t mappingCount(void *) {
  return 1;
}
bool mappingAt(void *Opaque, uint64_t Index, rp_address_space_mapping *Output) {
  if (Index != 0)
    return false;
  auto &Space = *static_cast<AddressSpace *>(Opaque);
  *Output = { Space.Entry.c_str(),
              Space.Bytes.size(),
              Space.Bytes.data(),
              Space.Bytes.size(),
              true,
              false,
              true,
              "reference-code" };
  return true;
}
uint64_t extraCodeCount(void *) {
  return 0;
}

void printError(rp_error *Error) {
  if (rp_error_is_success(Error))
    return;
  if (rp_error_is_document_error(Error)) {
    rp_document_error *Document = rp_error_get_document_error(Error);
    llvm::errs() << rp_document_error_get_error_type(Document) << "\n";
  } else {
    rp_simple_error *Simple = rp_error_get_simple_error(Error);
    llvm::errs() << rp_simple_error_get_message(Simple) << "\n";
  }
}

} // namespace

BOOST_AUTO_TEST_CASE(IsolateThroughEmitC) {
  const std::string PipelineOption = "--pipeline-path="
                                     + std::string(REVNG_SOURCE_DIR)
                                     + "/share/revng/pipelines/"
                                       "revng-pipelines.yml";
  const char *Arguments[] = { "test-reference-end-to-end",
                              PipelineOption.c_str() };
  BOOST_REQUIRE(rp_initialize(2, Arguments, 0, nullptr));

  AddressSpace Space;
  rp_address_space_callbacks Callbacks{ &Space,     architecture,
                                        entryPoint, mappingCount,
                                        mappingAt,  extraCodeCount,
                                        nullptr };
  std::unique_ptr<rp_error, decltype(&rp_error_destroy)>
    Error(rp_error_create(), rp_error_destroy);
  std::unique_ptr<rp_manager, decltype(&rp_manager_destroy)>
    Manager(rp_manager_create_from_address_space(&Callbacks,
                                                 0,
                                                 nullptr,
                                                 "",
                                                 Error.get()),
            rp_manager_destroy);
  if (Manager == nullptr)
    printError(Error.get());
  BOOST_REQUIRE(Manager != nullptr);
  const rp_primitive_type I32{ RP_PRIMITIVE_KIND_SIGNED, 4 };
  const rp_cabi_argument AddArguments[] = { { "a", I32 }, { "b", I32 } };
  BOOST_REQUIRE(rp_manager_set_cabi_prototype(Manager.get(),
                                              Space.Entry.c_str(),
                                              "SystemV_x86_64",
                                              "add",
                                              2,
                                              AddArguments,
                                              &I32,
                                              Error.get()));
  BOOST_REQUIRE(rp_manager_set_lifter_backend(Manager.get(),
                                              "reference-x86_64",
                                              Error.get()));

  rp_step *Step = rp_manager_get_step_from_name(Manager.get(), "emit-c");
  const rp_container_identifier
    *Identifier = rp_manager_get_container_identifier_from_name(Manager.get(),
                                                                "decompiled."
                                                                "tar.gz");
  const rp_kind *Kind = rp_manager_get_kind_from_name(Manager.get(),
                                                      "decompiled");
  BOOST_REQUIRE(Step != nullptr);
  BOOST_REQUIRE(Identifier != nullptr);
  BOOST_REQUIRE(Kind != nullptr);
  rp_container *Container = rp_step_get_container(Step, Identifier);
  const char *Path[] = { Space.Entry.c_str() };
  std::unique_ptr<rp_target, decltype(&rp_target_destroy)>
    Target(rp_target_create(Kind, 1, Path), rp_target_destroy);
  const rp_target *Targets[] = { Target.get() };
  std::unique_ptr<rp_buffer, decltype(&rp_buffer_destroy)>
    Output(rp_manager_produce_targets(Manager.get(),
                                      Step,
                                      Container,
                                      1,
                                      Targets,
                                      Error.get()),
           rp_buffer_destroy);
  if (Output == nullptr)
    printError(Error.get());
  BOOST_REQUIRE(Output != nullptr);
  BOOST_CHECK(rp_buffer_size(Output.get()) != 0U);

  Manager.reset();
  BOOST_CHECK(rp_shutdown());
}
