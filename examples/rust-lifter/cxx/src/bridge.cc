#include "revng-rust-lifter-example/cxx/include/bridge.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "llvm-c/Types.h"

// Use the public C view of PipelineC. Including its C++ view would pull the
// C++20 revng API into cxx's C++17 compatibility translation unit.
#define REVNG_PIPELINEC_C_ONLY
#include "revng/PipelineC/PipelineC.h"
#undef REVNG_PIPELINEC_C_ONLY

#include "revng-rust-lifter-example/src/main.rs.h"

struct revng_rust_decoded_instruction {
  std::uint64_t Address;
  std::uint8_t Kind;
};

extern "C" const char *
revng_rust_emit_x86_64(std::uintptr_t Output,
                       const revng_rust_decoded_instruction *Instructions,
                       std::size_t InstructionCount, std::uint64_t Entry);

namespace revng_rust {
namespace {

struct CallbackContext {
  RustBackend *Backend = nullptr;
  std::string Error;
  std::uint8_t Bytes[2] = {0x90, 0xc3};
};

const char *architecture(void *) { return "x86_64"; }

const char *entryPoint(void *) { return "0x400000:Code_x86_64"; }

std::uint64_t mappingCount(void *) { return 1; }

bool mappingAt(void *Opaque, std::uint64_t Index,
               rp_address_space_mapping *Output) {
  if (Index != 0)
    return false;
  auto &Context = *static_cast<CallbackContext *>(Opaque);
  Output->start = "0x400000:Code_x86_64";
  Output->virtual_size = 4;
  Output->backing_size = sizeof(Context.Bytes);
  Output->readable = true;
  Output->writeable = false;
  Output->executable = true;
  Output->name = "rust-example-code";
  return true;
}

bool readBytes(void *Opaque, std::uint64_t MappingIndex,
               std::uint64_t Offset, std::uint8_t *Destination,
               std::uint64_t Size) {
  auto &Context = *static_cast<CallbackContext *>(Opaque);
  if (MappingIndex != 0 or Offset > sizeof(Context.Bytes)
      or Size > sizeof(Context.Bytes) - Offset)
    return false;
  std::copy_n(Context.Bytes + Offset, Size, Destination);
  return true;
}

std::uint64_t extraCodeAddressCount(void *) { return 0; }

bool liftCallback(void *Opaque, const char *ModelYAML,
                  const rp_binary_view *Binary,
                  const char *const Entries[], std::uint64_t EntryCount,
                  LLVMModuleRef Output, const char **ErrorMessage) {
  auto &Context = *static_cast<CallbackContext *>(Opaque);
  std::vector<std::uint8_t> Bytes(rp_binary_view_size(Binary));
  rp_error *ReadError = rp_error_create();
  bool Read = rp_binary_view_read_offset(Binary, 0, Bytes.size(), Bytes.data(),
                                         ReadError);
  rp_error_destroy(ReadError);
  if (not Read) {
    Context.Error = "failed to read the input address space";
    *ErrorMessage = Context.Error.c_str();
    return false;
  }
  rust::Vec<rust::String> RustEntries;
  RustEntries.reserve(EntryCount);
  for (std::uint64_t I = 0; I < EntryCount; ++I)
    RustEntries.emplace_back(Entries[I]);

  rust::String Result =
      lift(*Context.Backend, rust::Str(ModelYAML),
           rust::Slice<const std::uint8_t>(Bytes.data(), Bytes.size()),
           std::move(RustEntries), reinterpret_cast<std::uintptr_t>(Output));
  Context.Error.assign(Result.data(), Result.size());
  if (Context.Error.empty())
    return true;
  *ErrorMessage = Context.Error.c_str();
  return false;
}

std::string errorMessage(rp_error *Error, const char *Fallback) {
  if (Error != nullptr) {
    if (auto *Simple = rp_error_get_simple_error(Error)) {
      if (const char *Message = rp_simple_error_get_message(Simple))
        return Message;
    }
    if (auto *Document = rp_error_get_document_error(Error)) {
      if (rp_document_error_reasons_count(Document) != 0)
        if (const char *Message =
                rp_document_error_get_error_message(Document, 0))
          return Message;
    }
  }
  return Fallback;
}

} // namespace

rust::String emit_x86_64(std::uintptr_t OutputAddress,
                         rust::Slice<const DecodedInstruction> Instructions,
                         std::uint64_t Entry) {
  std::vector<revng_rust_decoded_instruction> Decoded;
  Decoded.reserve(Instructions.size());
  for (const DecodedInstruction &Instruction : Instructions)
    Decoded.push_back({Instruction.address, Instruction.kind});
  const char *Error = revng_rust_emit_x86_64(OutputAddress, Decoded.data(),
                                             Decoded.size(), Entry);
  return Error == nullptr ? rust::String() : rust::String(Error);
}

rust::String run(RustBackend &Backend, rust::Str PipelinePath,
                 rust::Str BackendName) {
  std::string Pipeline(PipelinePath.data(), PipelinePath.size());
  std::string Name(BackendName.data(), BackendName.size());
  std::string PipelineOption = "--pipeline-path=" + Pipeline;
  const char *Arguments[] = {"revng-rust-lifter-example",
                             PipelineOption.c_str()};
  if (not rp_initialize(2, Arguments, 0, nullptr))
    return rust::String("rp_initialize failed");

  CallbackContext Context{.Backend = &Backend};
  rp_address_space_callbacks AddressSpace = {
      &Context,  architecture,          entryPoint, mappingCount,
      mappingAt, readBytes, extraCodeAddressCount, nullptr, nullptr};
  rp_error *Error = rp_error_create();
  rp_manager *Manager = rp_manager_create_from_address_space(
      &AddressSpace, 0, 0, nullptr, "", Error);
  std::string Result;
  if (Manager == nullptr) {
    Result = errorMessage(Error, "failed to create revng manager");
  } else {
    bool Selected = false;
    if (Name == "rust") {
      rp_lifter_callbacks Lifter{&Context, liftCallback};
      Selected = rp_set_lifter(Manager, &Lifter, Error);
    } else {
      Selected = rp_manager_set_lifter_backend(Manager, Name.c_str(), Error);
    }
    if (not Selected) {
      Result = errorMessage(Error, "failed to select lifter backend");
    } else {
      rp_step *Step = rp_manager_get_step_from_name(Manager, "lift");
      const rp_container_identifier *Identifier =
          rp_manager_get_container_identifier_from_name(Manager,
                                                        "root.bc.zstd");
      const rp_kind *Kind = rp_manager_get_kind_from_name(Manager, "root");
      if (Step == nullptr or Identifier == nullptr or Kind == nullptr) {
        Result = "the example pipeline has no lift/root output";
      } else {
        rp_container *Container = rp_step_get_container(Step, Identifier);
        const char *Path[] = {nullptr};
        rp_target *Target = rp_target_create(Kind, 0, Path);
        const rp_target *Targets[] = {Target};
        rp_buffer *Module = rp_manager_produce_targets(Manager, Step, Container,
                                                       1, Targets, Error);
        rp_target_destroy(Target);
        if (Module == nullptr) {
          Result = errorMessage(Error, "lift failed");
        } else {
          const std::uint64_t Size = rp_buffer_size(Module);
          if (Size == 0)
            Result = "revng produced an empty module";
          else
            std::cout << Name << " produced " << Size
                      << " bytes of LLVM module\n";
          rp_buffer_destroy(Module);
        }
      }
    }
    rp_manager_destroy(Manager);
  }
  rp_error_destroy(Error);
  if (not rp_shutdown() and Result.empty())
    Result = "rp_shutdown failed";
  return rust::String(Result.data(), Result.size());
}

} // namespace revng_rust
