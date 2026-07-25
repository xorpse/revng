//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CBindingWrapping.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/ABI/DefaultFunctionPrototype.h"
#include "revng/Lift/AbstractLifter.h"
#include "revng/Loader/AddressSpaceLoader.h"
#include "revng/Model/ArrayType.h"
#include "revng/Model/PointerType.h"
#include "revng/Model/PrimitiveKind.h"
#include "revng/Model/RawBinaryProvider.h"
#include "revng/Model/RawFunctionDefinition.h"
#include "revng/Model/Register.h"
#include "revng/Pipeline/AllRegistries.h"
#include "revng/Pipeline/Container.h"
#include "revng/Pipeline/Runner.h"
#include "revng/Pipeline/Target.h"
#include "revng/PipelineC/PipelineC.h"
#include "revng/PipelineC/Tracing/Private.h"
#include "revng/Pipes/ModelGlobal.h"
#include "revng/Pipes/PipelineManager.h"
#include "revng/Support/Assert.h"
#include "revng/Support/GzipTarFile.h"
#include "revng/Support/InitRevng.h"
#include "revng/Support/ResourceFinder.h"
#include "revng/TupleTree/TupleTreeDiff.h"

#include "Tracing/Wrapper.h"

using namespace pipeline;
using namespace ::revng::pipes;
namespace cl = llvm::cl;

template <typename T>
concept default_constructible = std::is_default_constructible<T>::value;

template <default_constructible T> class ExistingOrNew {
private:
  T *Pointer = nullptr;
  std::optional<T> Default;

public:
  explicit ExistingOrNew(T *Pointer) {
    if (Pointer != nullptr) {
      this->Pointer = Pointer;
    } else {
      Default.emplace();
      this->Pointer = &*Default;
    }
  }

  ExistingOrNew(ExistingOrNew &&) = delete;
  ExistingOrNew(const ExistingOrNew &) = delete;
  ExistingOrNew &operator=(ExistingOrNew &&) = delete;
  ExistingOrNew &operator=(const ExistingOrNew &) = delete;
  ~ExistingOrNew() = default;

  T &operator*() { return *Pointer; }
  T *operator->() { return Pointer; }
};

static void llvmErrorToRpError(llvm::Error Error, rp_error *Out) {
  if (Out == nullptr) {
    // By passing `Out == nullptr` the callee indicated that they do not care
    // about the error, so we can just discard it.
    //
    // For more context, see `include/revng/PipelineC/Prototypes.h`.
    llvm::consumeError(std::move(Error));
    return;
  }

  auto DocumentedErrorHandler = [&Out](const revng::DocumentErrorBase &Error) {
    rp_document_error ErrorBody(Error.getTypeName(),
                                Error.getLocationTypeName());

    for (size_t I = 0; I < Error.size(); I++) {
      rp_error_reason Reason(Error.getMessage(I), Error.getLocation(I));
      ErrorBody.Reasons.emplace_back(Reason);
    }

    *Out = std::move(ErrorBody);
  };

  auto OtherErrorHandler = [&Out](const llvm::ErrorInfoBase &OtherErrors) {
    std::string Reason;
    llvm::raw_string_ostream OS(Reason);
    OtherErrors.log(OS);
    OS.flush();
    *Out = rp_simple_error(Reason, "");
  };

  llvm::handleAllErrors(std::move(Error), DocumentedErrorHandler,
                        OtherErrorHandler);
}

void revng::tracing::setTracing(llvm::raw_ostream *OS) { Tracing.swap(OS); }

/// Used when we want to return a stack allocated string. Copies the string onto
/// the heap and gives ownership of to the caller
static char *copyString(llvm::StringRef Str) {
  char *ToReturn = (char *)malloc(sizeof(char) * (Str.size() + 1));
  strncpy(ToReturn, Str.data(), Str.size());
  ToReturn[Str.size()] = 0;
  return ToReturn;
}

static bool Initialized = false;
static std::optional<revng::InitRevng> InitRevngInstance = std::nullopt;
static cl::list<std::string> PipelinePaths("pipeline-path", cl::ZeroOrMore);
typedef void (*sighandler_t)(int);

// NOLINTBEGIN

static bool _rp_initialize(int argc, const char *argv[],
                           uint32_t signals_to_preserve_count,
                           int signals_to_preserve[]) {
  if (argc != 0)
    revng_check(argv != nullptr);

  if (Initialized)
    return false;

  std::map<int, sighandler_t> Signals;
  for (uint32_t I = 0; I < signals_to_preserve_count; I++) {
    // For each signal number we are asked to preserve we need to extract the
    // function pointer to the signal and save it. The constructor for
    // revng::InitRevng will call LLVM's RegisterHandlers which overrides most
    // signal handlers and chains the previous one afterwards, we want instead
    // to keep the already existing handler and remove the LLVM's one
    int SigNumber = signals_to_preserve[I];
    sighandler_t Handler = signal(SigNumber, SIG_DFL);
    if (Handler != SIG_ERR && Handler != NULL) {
      // We save the signal handler for restoration after we initialize LLVM's
      // machinery, as said we do not restore the signal to avoid LLVM chaining
      // it after its own
      Signals[SigNumber] = Handler;
    }
  }

  revng_check(not InitRevngInstance.has_value());
  char **MutableArgv = const_cast<char **>(argv);
  InitRevngInstance.emplace(argc, MutableArgv, "",
                            llvm::ArrayRef<llvm::cl::OptionCategory *>());

  for (const auto &[SigNumber, Handler] : Signals) {
    // All of LLVM's initialization is complete, restore the original signals to
    // the respective signal number
    signal(SigNumber, Handler);
  }

  Initialized = true;
  Registry::runAllInitializationRoutines();

  return true;
}

static bool _rp_shutdown() {
  if (InitRevngInstance.has_value()) {
    InitRevngInstance.reset();
    return true;
  }
  return false;
}

static rp_manager *rp_manager_create_impl(llvm::ArrayRef<std::string> Pipelines,
                                          uint64_t pipeline_flags_count,
                                          const char *pipeline_flags[],
                                          const char *execution_directory,
                                          bool is_path = true) {
  revng_check(Initialized);
  if (pipeline_flags == nullptr)
    revng_check(pipeline_flags_count == 0);
  revng_check(execution_directory != nullptr);

  std::vector<std::string> FlagsVector;
  for (size_t I = 0; I < pipeline_flags_count; I++)
    FlagsVector.push_back(pipeline_flags[I]);

  using PM = PipelineManager;
  auto MaybePipeline =
      is_path
          ? PM::create(Pipelines, FlagsVector, execution_directory)
          : PM::createFromMemory(Pipelines, FlagsVector, execution_directory);
  if (auto Error = MaybePipeline.takeError()) {
    llvm::errs() << "Pipeline creation failed: " << Error << "\n";
    llvm::consumeError(std::move(Error));
    return nullptr;
  }

  auto manager = new PipelineManager(std::move(*MaybePipeline));
  return manager;
}

static rp_manager *_rp_manager_create_from_string(
    uint64_t pipelines_count, const char *pipelines[],
    uint64_t pipeline_flags_count, const char *pipeline_flags[],
    const char *execution_directory) {
  std::vector<std::string> Pipelines;
  for (size_t I = 0; I < pipelines_count; I++)
    Pipelines.push_back(pipelines[I]);

  return rp_manager_create_impl(Pipelines, pipeline_flags_count, pipeline_flags,
                                execution_directory, false);
}

static rp_manager *_rp_manager_create(uint64_t pipeline_flags_count,
                                      const char *pipeline_flags[],
                                      const char *execution_directory) {
  return rp_manager_create_impl(PipelinePaths, pipeline_flags_count,
                                pipeline_flags, execution_directory, true);
}

namespace {

class CallbackAddressSpace final : public revng::loader::AbstractAddressSpace {
private:
  model::Architecture::Values Architecture;
  std::optional<MetaAddress> EntryPoint;
  std::vector<revng::loader::Mapping> Mappings;
  std::vector<MetaAddress> ExtraCodeAddresses;

public:
  CallbackAddressSpace(model::Architecture::Values Architecture,
                       std::optional<MetaAddress> EntryPoint,
                       std::vector<revng::loader::Mapping> Mappings,
                       std::vector<MetaAddress> ExtraCodeAddresses)
      : Architecture(Architecture), EntryPoint(EntryPoint),
        Mappings(std::move(Mappings)),
        ExtraCodeAddresses(std::move(ExtraCodeAddresses)) {}

  model::Architecture::Values architecture() const override {
    return Architecture;
  }
  std::optional<MetaAddress> entryPoint() const override { return EntryPoint; }
  llvm::ArrayRef<revng::loader::Mapping> mappings() const override {
    return Mappings;
  }
  llvm::ArrayRef<MetaAddress> extraCodeAddresses() const override {
    return ExtraCodeAddresses;
  }
};

class CallbackLifter final : public revng::lift::ILifter {
private:
  rp_lifter_callbacks Callbacks;
  const TupleTree<model::Binary> &Model;

public:
  CallbackLifter(rp_lifter_callbacks Callbacks,
                 const TupleTree<model::Binary> &Model)
      : Callbacks(Callbacks), Model(Model) {}

  llvm::Error lift(const model::Binary &, const RawBinaryView &View,
                   llvm::ArrayRef<MetaAddress> Entries,
                   llvm::Module &Output) override {
    std::vector<std::string> EntryStrings;
    std::vector<const char *> EntryPointers;
    EntryStrings.reserve(Entries.size());
    EntryPointers.reserve(Entries.size());
    for (MetaAddress Entry : Entries)
      EntryStrings.push_back(Entry.toString());
    for (const std::string &Entry : EntryStrings)
      EntryPointers.push_back(Entry.c_str());

    const char *ErrorMessage = nullptr;
    bool Success = Callbacks.lift(
        Callbacks.opaque, &Model, &View, EntryPointers.data(),
        EntryPointers.size(), llvm::wrap(&Output), &ErrorMessage);
    if (Success)
      return llvm::Error::success();
    return revng::createError(ErrorMessage == nullptr ? "host lifter callback "
                                                        "failed"
                                                      : ErrorMessage);
  }
};

struct LazyMappingDescriptor {
  uint64_t StartOffset;
  uint64_t VirtualSize;
  uint64_t BackingSize;
  uint64_t CallbackIndex;
};

class CallbackRawBinaryProvider final : public RawBinaryProvider {
private:
  rp_address_space_callbacks Callbacks;
  std::vector<LazyMappingDescriptor> Mappings;
  uint64_t TotalSize = 0;
  mutable std::mutex Mutex;
  mutable std::map<std::pair<uint64_t, uint64_t>, std::vector<uint8_t>> Cache;

public:
  explicit CallbackRawBinaryProvider(rp_address_space_callbacks Callbacks)
      : Callbacks(Callbacks) {}

  ~CallbackRawBinaryProvider() override {
    if (Callbacks.release != nullptr)
      Callbacks.release(Callbacks.opaque);
  }

  void addMapping(uint64_t VirtualSize, uint64_t BackingSize,
                  uint64_t CallbackIndex) {
    Mappings.push_back({TotalSize, VirtualSize, BackingSize, CallbackIndex});
    TotalSize += VirtualSize;
  }

  uint64_t size() const override { return TotalSize; }

  std::optional<llvm::ArrayRef<uint8_t>>
  getByOffset(uint64_t Offset, uint64_t Size) const override {
    if (Offset > TotalSize or Size > TotalSize - Offset or
        Size > std::numeric_limits<size_t>::max())
      return std::nullopt;

    std::lock_guard Lock(Mutex);
    auto Key = std::pair(Offset, Size);
    auto Existing = Cache.find(Key);
    if (Existing != Cache.end())
      return Existing->second;

    std::vector<uint8_t> Result(size_t(Size), 0);
    const uint64_t End = Offset + Size;
    for (const LazyMappingDescriptor &Mapping : Mappings) {
      const uint64_t BackedStart = Mapping.StartOffset;
      const uint64_t BackedEnd = BackedStart + Mapping.BackingSize;
      const uint64_t ReadStart = std::max(Offset, BackedStart);
      const uint64_t ReadEnd = std::min(End, BackedEnd);
      if (ReadStart >= ReadEnd)
        continue;
      const uint64_t ReadSize = ReadEnd - ReadStart;
      if (not Callbacks.read(
              Callbacks.opaque, Mapping.CallbackIndex, ReadStart - BackedStart,
              Result.data() + size_t(ReadStart - Offset), ReadSize))
        return std::nullopt;
    }

    auto [Iterator, Inserted] = Cache.emplace(Key, std::move(Result));
    revng_assert(Inserted);
    return Iterator->second;
  }
};

struct FileMappingDescriptor {
  std::string Start;
  uint64_t VirtualSize;
  uint64_t BackingSize;
  std::string Path;
  uint64_t FileOffset;
  bool Readable;
  bool Writeable;
  bool Executable;
  std::string Name;
};

struct FileAddressSpaceContext {
  std::string Architecture;
  std::string EntryPoint;
  std::vector<FileMappingDescriptor> Mappings;
};

const char *fileArchitecture(void *Opaque) {
  return static_cast<FileAddressSpaceContext *>(Opaque)->Architecture.c_str();
}

const char *fileEntryPoint(void *Opaque) {
  auto &Entry = static_cast<FileAddressSpaceContext *>(Opaque)->EntryPoint;
  return Entry.empty() ? nullptr : Entry.c_str();
}

uint64_t fileMappingCount(void *Opaque) {
  return static_cast<FileAddressSpaceContext *>(Opaque)->Mappings.size();
}

bool fileMappingAt(void *Opaque, uint64_t Index,
                   rp_address_space_mapping *Output) {
  auto &Mappings = static_cast<FileAddressSpaceContext *>(Opaque)->Mappings;
  if (Index >= Mappings.size() or Output == nullptr)
    return false;
  const FileMappingDescriptor &Mapping = Mappings[Index];
  *Output = {Mapping.Start.c_str(), Mapping.VirtualSize, Mapping.BackingSize,
             Mapping.Readable,      Mapping.Writeable,   Mapping.Executable,
             Mapping.Name.c_str()};
  return true;
}

bool fileRead(void *Opaque, uint64_t MappingIndex, uint64_t Offset,
              uint8_t *Destination, uint64_t Size) {
  auto &Mappings = static_cast<FileAddressSpaceContext *>(Opaque)->Mappings;
  if (MappingIndex >= Mappings.size() or (Size != 0 and Destination == nullptr))
    return false;
  const FileMappingDescriptor &Mapping = Mappings[MappingIndex];
  if (Offset > Mapping.BackingSize or Size > Mapping.BackingSize - Offset or
      Mapping.FileOffset > std::numeric_limits<uint64_t>::max() - Offset or
      Mapping.FileOffset + Offset >
          uint64_t(std::numeric_limits<std::streamoff>::max()) or
      Size > std::numeric_limits<std::streamsize>::max())
    return false;
  std::ifstream Input(Mapping.Path, std::ios::binary);
  if (not Input)
    return false;
  Input.seekg(std::streamoff(Mapping.FileOffset + Offset));
  if (not Input)
    return false;
  Input.read(reinterpret_cast<char *>(Destination), std::streamsize(Size));
  return Input.good() or (Input.eof() and uint64_t(Input.gcount()) == Size);
}

void releaseFileAddressSpace(void *Opaque) {
  delete static_cast<FileAddressSpaceContext *>(Opaque);
}

} // namespace

static rp_manager *createManagerFromLoadedAddressSpace(
    revng::loader::LoadedAddressSpace Loaded,
    std::shared_ptr<RawBinaryProvider> Provider, uint64_t pipeline_flags_count,
    const char *pipeline_flags[], const char *execution_directory,
    rp_error *error) {
  std::unique_ptr<rp_manager> Manager(_rp_manager_create(
      pipeline_flags_count, pipeline_flags, execution_directory));
  if (not Manager)
    return nullptr;

  auto LoadedModel = std::move(Loaded.Model);
  // An entry point supplied by an embedding application is also a known
  // function entry. Seed it here so PipelineC consumers can drive pipelines
  // beyond lifting without reaching into revng's C++ model implementation.
  if (LoadedModel->EntryPoint().isValid() and
      LoadedModel->DefaultABI() != model::ABI::Invalid) {
    LoadedModel->DefaultPrototype() =
        abi::registerDefaultFunctionPrototype(*LoadedModel);
    model::Function Function;
    Function.Entry() = LoadedModel->EntryPoint();
    LoadedModel->Functions().insert(std::move(Function));
  }
  auto &WritableModel = revng::getWritableModelFromContext(Manager->context());
  WritableModel = std::move(LoadedModel);

  llvm::StringRef Bytes(reinterpret_cast<const char *>(Loaded.Data.data()),
                        Loaded.Data.size());
  auto Buffer =
      llvm::MemoryBuffer::getMemBuffer(Bytes, "abstract-address-space", false);
  pipeline::Step &Begin = *Manager->getRunner().begin();
  auto Invalidations = Manager->deserializeContainer(Begin, "input", *Buffer);
  if (not Invalidations) {
    llvmErrorToRpError(Invalidations.takeError(), error);
    return nullptr;
  }
  if (Provider) {
    const auto &ReadOnlyModel = revng::getModelFromContext(Manager->context());
    registerRawBinaryProvider(*ReadOnlyModel, std::move(Provider));
  }
  Manager->recalculateAllPossibleTargets();
  return Manager.release();
}

static rp_manager *_rp_manager_create_from_address_space(
    const rp_address_space_callbacks *callbacks,
    uint64_t materialize_for_serialization, uint64_t pipeline_flags_count,
    const char *pipeline_flags[], const char *execution_directory,
    rp_error *error) {
  revng_check(callbacks != nullptr);
  if (callbacks->architecture == nullptr or
      callbacks->mapping_count == nullptr or callbacks->mapping_at == nullptr or
      callbacks->read == nullptr) {
    llvmErrorToRpError(revng::createError("incomplete lazy address-space "
                                          "callback table"),
                       error);
    return nullptr;
  }

  auto Provider = std::make_shared<CallbackRawBinaryProvider>(*callbacks);
  const char *ArchitectureName = callbacks->architecture(callbacks->opaque);
  if (ArchitectureName == nullptr) {
    llvmErrorToRpError(revng::createError("address-space architecture callback "
                                          "returned null"),
                       error);
    return nullptr;
  }
  auto Architecture = model::Architecture::fromName(ArchitectureName);
  if (Architecture == model::Architecture::Invalid) {
    llvmErrorToRpError(
        revng::createError("unknown address-space architecture: " +
                           llvm::StringRef(ArchitectureName)),
        error);
    return nullptr;
  }

  std::optional<MetaAddress> EntryPoint;
  if (callbacks->entry_point != nullptr) {
    if (const char *Value = callbacks->entry_point(callbacks->opaque)) {
      EntryPoint = MetaAddress::fromString(Value);
      if (EntryPoint->isInvalid()) {
        llvmErrorToRpError(revng::createError("invalid address-space entry "
                                              "point"),
                           error);
        return nullptr;
      }
    }
  }

  std::vector<revng::loader::Mapping> Mappings;
  const uint64_t MappingCount = callbacks->mapping_count(callbacks->opaque);
  Mappings.reserve(MappingCount);
  uint64_t TotalSize = 0;
  for (uint64_t I = 0; I < MappingCount; ++I) {
    rp_address_space_mapping Input{};
    if (not callbacks->mapping_at(callbacks->opaque, I, &Input)) {
      llvmErrorToRpError(revng::createError("lazy address-space mapping "
                                            "callback failed"),
                         error);
      return nullptr;
    }
    if (Input.start == nullptr) {
      llvmErrorToRpError(revng::createError("address-space mapping has no "
                                            "start address"),
                         error);
      return nullptr;
    }
    if (Input.backing_size > Input.virtual_size) {
      llvmErrorToRpError(revng::createError("address-space mapping backing "
                                            "size exceeds its virtual size"),
                         error);
      return nullptr;
    }
    if (Input.virtual_size > std::numeric_limits<uint64_t>::max() - TotalSize) {
      llvmErrorToRpError(revng::createError("flattened address-space buffer is "
                                            "too large"),
                         error);
      return nullptr;
    }
    MetaAddress Start = MetaAddress::fromString(Input.start);
    if (Start.isInvalid()) {
      llvmErrorToRpError(revng::createError("address-space mapping has invalid "
                                            "start address"),
                         error);
      return nullptr;
    }
    Mappings.push_back({Start,
                        Input.virtual_size,
                        {},
                        Input.readable,
                        Input.writeable,
                        Input.executable,
                        Input.name == nullptr ? "" : Input.name,
                        Input.backing_size});
    Provider->addMapping(Input.virtual_size, Input.backing_size, I);
    TotalSize += Input.virtual_size;
  }

  std::vector<MetaAddress> ExtraCodeAddresses;
  if (callbacks->extra_code_address_count != nullptr) {
    const uint64_t Count =
        callbacks->extra_code_address_count(callbacks->opaque);
    if (Count != 0 and callbacks->extra_code_address_at == nullptr) {
      llvmErrorToRpError(revng::createError("incomplete extra-code-address "
                                            "callbacks"),
                         error);
      return nullptr;
    }
    ExtraCodeAddresses.reserve(Count);
    for (uint64_t I = 0; I < Count; ++I) {
      const char *Value =
          callbacks->extra_code_address_at(callbacks->opaque, I);
      if (Value == nullptr) {
        llvmErrorToRpError(revng::createError("extra code address callback "
                                              "returned null"),
                           error);
        return nullptr;
      }
      MetaAddress Address = MetaAddress::fromString(Value);
      if (Address.isInvalid()) {
        llvmErrorToRpError(revng::createError("invalid extra code address"),
                           error);
        return nullptr;
      }
      ExtraCodeAddresses.push_back(Address);
    }
  }

  CallbackAddressSpace AddressSpace(Architecture, EntryPoint,
                                    std::move(Mappings),
                                    std::move(ExtraCodeAddresses));
  auto Loaded = revng::loader::loadAddressSpace(AddressSpace, false);
  if (not Loaded) {
    llvmErrorToRpError(Loaded.takeError(), error);
    return nullptr;
  }

  if (materialize_for_serialization) {
    auto Bytes = Provider->bytes();
    if (not Bytes) {
      llvmErrorToRpError(revng::createError("lazy address-space read failed "
                                            "during materialization"),
                         error);
      return nullptr;
    }
    Loaded->Data.assign(Bytes->begin(), Bytes->end());
    Provider.reset();
  }

  return createManagerFromLoadedAddressSpace(
      std::move(*Loaded), std::move(Provider), pipeline_flags_count,
      pipeline_flags, execution_directory, error);
}

static rp_manager *_rp_manager_create_from_file_address_space(
    const char *architecture, const char *entry_point, uint64_t mappings_count,
    const rp_file_address_space_mapping mappings[],
    uint64_t materialize_for_serialization, uint64_t pipeline_flags_count,
    const char *pipeline_flags[], const char *execution_directory,
    rp_error *error) {
  revng_check(architecture != nullptr);
  if (mappings_count != 0 and mappings == nullptr) {
    llvmErrorToRpError(revng::createError("null file mapping array"), error);
    return nullptr;
  }
  auto Context = std::make_unique<FileAddressSpaceContext>();
  Context->Architecture = architecture;
  if (entry_point != nullptr)
    Context->EntryPoint = entry_point;
  Context->Mappings.reserve(mappings_count);
  for (uint64_t I = 0; I < mappings_count; ++I) {
    if (mappings[I].start == nullptr or mappings[I].path == nullptr) {
      llvmErrorToRpError(revng::createError("file mapping has a null start or "
                                            "path"),
                         error);
      return nullptr;
    }
    Context->Mappings.push_back(
        {mappings[I].start, mappings[I].virtual_size, mappings[I].backing_size,
         mappings[I].path, mappings[I].file_offset, mappings[I].readable,
         mappings[I].writeable, mappings[I].executable,
         mappings[I].name == nullptr ? "" : mappings[I].name});
  }
  rp_address_space_callbacks Callbacks{
      Context.get(),    fileArchitecture, fileEntryPoint,
      fileMappingCount, fileMappingAt,    fileRead,
      nullptr,          nullptr,          releaseFileAddressSpace};
  Context.release();
  return _rp_manager_create_from_address_space(
      &Callbacks, materialize_for_serialization, pipeline_flags_count,
      pipeline_flags, execution_directory, error);
}

static bool _rp_set_lifter(rp_manager *manager,
                           const rp_lifter_callbacks *callbacks,
                           rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(callbacks != nullptr);
  if (callbacks->lift == nullptr) {
    llvmErrorToRpError(revng::createError("host lifter callback is null"),
                       error);
    return false;
  }

  rp_lifter_callbacks Copy = *callbacks;
  const auto &Model = revng::getModelFromContext(manager->context());
  revng::lift::LifterFactory Factory =
      [Copy](const TupleTree<model::Binary> &FactoryModel) {
        return std::make_unique<CallbackLifter>(Copy, FactoryModel);
      };
  llvm::Error Result =
      revng::lift::LifterRegistry::setOverride(*Model, std::move(Factory));
  if (Result) {
    llvmErrorToRpError(std::move(Result), error);
    return false;
  }
  return true;
}

static uint64_t _rp_binary_view_size(const rp_binary_view *binary) {
  revng_check(binary != nullptr);
  return binary->size();
}

static bool _rp_binary_view_read_offset(const rp_binary_view *binary,
                                        uint64_t offset, uint64_t size,
                                        uint8_t destination[],
                                        rp_error *error) {
  revng_check(binary != nullptr);
  if (size != 0 and destination == nullptr) {
    llvmErrorToRpError(revng::createError("null binary read destination"),
                       error);
    return false;
  }
  auto Bytes = binary->getByOffset(offset, size);
  if (not Bytes) {
    llvmErrorToRpError(revng::createError("binary offset read failed"), error);
    return false;
  }
  std::copy(Bytes->begin(), Bytes->end(), destination);
  return true;
}

static bool _rp_binary_view_read_address(const rp_binary_view *binary,
                                         const char *address, uint64_t size,
                                         uint8_t destination[],
                                         rp_error *error) {
  revng_check(binary != nullptr);
  revng_check(address != nullptr);
  if (size != 0 and destination == nullptr) {
    llvmErrorToRpError(revng::createError("null binary read destination"),
                       error);
    return false;
  }
  MetaAddress Address = MetaAddress::fromString(address);
  if (Address.isInvalid()) {
    llvmErrorToRpError(revng::createError("invalid binary read address"),
                       error);
    return false;
  }
  auto Bytes = binary->getByAddress(Address, size);
  if (not Bytes) {
    llvmErrorToRpError(revng::createError("binary address read failed"), error);
    return false;
  }
  std::copy(Bytes->begin(), Bytes->end(), destination);
  return true;
}

static bool _rp_manager_materialize_address_space(rp_manager *manager,
                                                  rp_error *error) {
  revng_check(manager != nullptr);
  const auto &Model = revng::getModelFromContext(manager->context());
  std::shared_ptr<RawBinaryProvider> Provider = findRawBinaryProvider(*Model);
  if (not Provider)
    return true;
  auto Bytes = Provider->bytes();
  if (not Bytes) {
    llvmErrorToRpError(revng::createError("lazy address-space "
                                          "materialization failed"),
                       error);
    return false;
  }
  llvm::StringRef String(reinterpret_cast<const char *>(Bytes->data()),
                         Bytes->size());
  auto Buffer = llvm::MemoryBuffer::getMemBuffer(
      String, "materialized-address-space", false);
  pipeline::Step &Begin = *manager->getRunner().begin();
  auto Invalidations = manager->deserializeContainer(Begin, "input", *Buffer);
  if (not Invalidations) {
    llvmErrorToRpError(Invalidations.takeError(), error);
    return false;
  }
  unregisterRawBinaryProvider(*Model);
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_set_lifter_backend(rp_manager *manager,
                                           const char *name, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);

  const std::string Name(name);
  const auto &Model = revng::getModelFromContext(manager->context());
  auto Probe = revng::lift::LifterRegistry::create(Name, Model);
  if (not Probe) {
    llvmErrorToRpError(Probe.takeError(), error);
    return false;
  }

  revng::lift::LifterFactory Factory =
      [Name](const TupleTree<model::Binary> &FactoryModel) {
        return llvm::cantFail(
            revng::lift::LifterRegistry::create(Name, FactoryModel));
      };
  llvm::Error Result =
      revng::lift::LifterRegistry::setOverride(*Model, std::move(Factory));
  if (Result) {
    llvmErrorToRpError(std::move(Result), error);
    return false;
  }
  return true;
}

static uint64_t _rp_lifter_backend_count() {
  return revng::lift::LifterRegistry::names().size();
}

static char *_rp_lifter_backend_name(uint64_t index) {
  std::vector<std::string> Names = revng::lift::LifterRegistry::names();
  return index < Names.size() ? copyString(Names[index]) : nullptr;
}

static bool _rp_lifter_backend_supports_architecture(const char *backend,
                                                     const char *architecture) {
  revng_check(backend != nullptr);
  revng_check(architecture != nullptr);
  auto Architecture = model::Architecture::fromName(architecture);
  if (Architecture == model::Architecture::Invalid)
    return false;
  return revng::lift::LifterRegistry::supportsArchitecture(backend,
                                                           Architecture);
}

static uint64_t _rp_architecture_count() {
  return uint64_t(model::Architecture::Count) - 1;
}

static char *_rp_architecture_name(uint64_t index) {
  if (index >= _rp_architecture_count())
    return nullptr;
  auto Architecture = model::Architecture::Values(index + 1);
  return copyString(model::Architecture::getName(Architecture));
}

static std::vector<model::ABI::Values>
abisForArchitecture(model::Architecture::Values Architecture) {
  std::vector<model::ABI::Values> Result;
  if (Architecture == model::Architecture::Invalid)
    return Result;
  for (uint64_t I = 1; I < uint64_t(model::ABI::Count); ++I) {
    auto ABI = model::ABI::Values(I);
    if (model::ABI::getArchitecture(ABI) == Architecture)
      Result.push_back(ABI);
  }
  return Result;
}

static uint64_t _rp_abi_count(const char *architecture) {
  revng_check(architecture != nullptr);
  return abisForArchitecture(model::Architecture::fromName(architecture))
      .size();
}

static char *_rp_abi_name(const char *architecture, uint64_t index) {
  revng_check(architecture != nullptr);
  auto ABIs = abisForArchitecture(model::Architecture::fromName(architecture));
  return index < ABIs.size() ? copyString(model::ABI::getName(ABIs[index]))
                             : nullptr;
}

static llvm::Expected<model::UpcastableType>
marshalPrimitiveType(const rp_primitive_type &Input, bool AllowVoid) {
  using PK = model::PrimitiveKind::Values;
  PK Kind = model::PrimitiveKind::Invalid;
  switch (Input.kind) {
  case RP_PRIMITIVE_KIND_VOID:
    Kind = model::PrimitiveKind::Void;
    break;
  case RP_PRIMITIVE_KIND_GENERIC:
    Kind = model::PrimitiveKind::Generic;
    break;
  case RP_PRIMITIVE_KIND_POINTER_OR_NUMBER:
    Kind = model::PrimitiveKind::PointerOrNumber;
    break;
  case RP_PRIMITIVE_KIND_NUMBER:
    Kind = model::PrimitiveKind::Number;
    break;
  case RP_PRIMITIVE_KIND_UNSIGNED:
    Kind = model::PrimitiveKind::Unsigned;
    break;
  case RP_PRIMITIVE_KIND_SIGNED:
    Kind = model::PrimitiveKind::Signed;
    break;
  case RP_PRIMITIVE_KIND_FLOAT:
    Kind = model::PrimitiveKind::Float;
    break;
  default:
    return revng::createError("invalid primitive kind");
  }

  if (Kind == model::PrimitiveKind::Void) {
    if (not AllowVoid or Input.size != 0)
      return revng::createError("void is not valid in this position");
    return model::PrimitiveType::makeVoid();
  }

  const bool GenericSize = Input.size == 1 or Input.size == 2 or
                           Input.size == 4 or Input.size == 8 or
                           Input.size == 16;
  const bool FloatSize = Input.size == 2 or Input.size == 4 or
                         Input.size == 8 or Input.size == 10 or
                         Input.size == 12 or Input.size == 16;
  if ((Kind == model::PrimitiveKind::Float and not FloatSize) or
      (Kind != model::PrimitiveKind::Float and not GenericSize))
    return revng::createError("invalid primitive size");
  return model::PrimitiveType::make(Kind, Input.size);
}

static model::TypeDefinition *findTypeDefinition(model::Binary &Model,
                                                 uint64_t ID) {
  for (model::UpcastableTypeDefinition &Definition : Model.TypeDefinitions())
    if (Definition->ID() == ID)
      return Definition.get();
  return nullptr;
}

static llvm::Expected<model::UpcastableType> marshalType(model::Binary &Model,
                                                         const rp_type &Input,
                                                         bool AllowVoid,
                                                         unsigned Depth = 0) {
  if (Depth == 64)
    return revng::createError("type expression is too deeply nested");

  model::UpcastableType Result;
  switch (Input.kind) {
  case RP_TYPE_KIND_PRIMITIVE: {
    auto Type = marshalPrimitiveType(Input.primitive, AllowVoid);
    if (not Type)
      return Type.takeError();
    Result = std::move(*Type);
    break;
  }
  case RP_TYPE_KIND_POINTER: {
    if (Input.element_type == nullptr)
      return revng::createError("pointer type has no pointee");
    auto Pointee = marshalType(Model, *Input.element_type, true, Depth + 1);
    if (not Pointee)
      return Pointee.takeError();
    const uint64_t PointerSize =
        Input.size == 0
            ? model::Architecture::getPointerSize(Model.Architecture())
            : Input.size;
    if (PointerSize == 0)
      return revng::createError("pointer type has an invalid size");
    Result = model::PointerType::make(std::move(*Pointee), PointerSize);
    break;
  }
  case RP_TYPE_KIND_ARRAY: {
    if (Input.element_type == nullptr)
      return revng::createError("array type has no element type");
    if (Input.size == 0)
      return revng::createError("array type has zero elements");
    auto Element = marshalType(Model, *Input.element_type, false, Depth + 1);
    if (not Element)
      return Element.takeError();
    Result = model::ArrayType::make(std::move(*Element), Input.size);
    break;
  }
  case RP_TYPE_KIND_DEFINED: {
    model::TypeDefinition *Definition =
        findTypeDefinition(Model, Input.definition_id);
    if (Definition == nullptr)
      return revng::createError("unknown type definition ID");
    Result = Model.makeType(Definition->key());
    break;
  }
  default:
    return revng::createError("invalid type kind");
  }
  Result->IsConst() = Input.is_const;
  return Result;
}

static bool _rp_manager_set_cabi_prototype(rp_manager *manager,
                                           const char *address, const char *abi,
                                           const char *function_name,
                                           uint64_t arguments_count,
                                           const rp_cabi_argument arguments[],
                                           const rp_primitive_type *return_type,
                                           rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(address != nullptr);
  revng_check(abi != nullptr);
  revng_check(function_name != nullptr);
  if (arguments_count != 0 and arguments == nullptr) {
    llvmErrorToRpError(revng::createError("null C ABI argument array"), error);
    return false;
  }

  MetaAddress Address = MetaAddress::fromString(address);
  if (Address.isInvalid()) {
    llvmErrorToRpError(revng::createError("invalid function address"), error);
    return false;
  }
  model::ABI::Values ABI = model::ABI::fromName(abi);
  if (ABI == model::ABI::Invalid) {
    llvmErrorToRpError(revng::createError("invalid ABI name"), error);
    return false;
  }

  std::vector<model::UpcastableType> ArgumentTypes;
  ArgumentTypes.reserve(arguments_count);
  for (uint64_t I = 0; I < arguments_count; ++I) {
    if (arguments[I].name == nullptr) {
      llvmErrorToRpError(revng::createError("null C ABI argument name"), error);
      return false;
    }
    auto Type = marshalPrimitiveType(arguments[I].type, false);
    if (not Type) {
      llvmErrorToRpError(Type.takeError(), error);
      return false;
    }
    ArgumentTypes.push_back(std::move(*Type));
  }
  std::optional<model::UpcastableType> ReturnType;
  if (return_type != nullptr) {
    const bool IsVoid = return_type->kind == RP_PRIMITIVE_KIND_VOID;
    auto Type = marshalPrimitiveType(*return_type, true);
    if (not Type) {
      llvmErrorToRpError(Type.takeError(), error);
      return false;
    }
    if (not IsVoid)
      ReturnType.emplace(std::move(*Type));
  }

  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto Function = Model->Functions().find(Address);
  if (Function == Model->Functions().end()) {
    llvmErrorToRpError(revng::createError("function address is not known"),
                       error);
    return false;
  }
  if (model::ABI::getArchitecture(ABI) != Model->Architecture()) {
    llvmErrorToRpError(revng::createError("ABI architecture does not match "
                                          "the address space"),
                       error);
    return false;
  }

  auto &&[Prototype, PrototypeType] = Model->makeCABIFunctionDefinition();
  Prototype.ABI() = ABI;
  for (uint64_t I = 0; I < arguments_count; ++I) {
    auto &Argument = Prototype.addArgument(std::move(ArgumentTypes[I]));
    Argument.Name() = arguments[I].name;
  }
  if (ReturnType.has_value())
    Prototype.ReturnType() = std::move(*ReturnType);
  Function->Name() = function_name;
  Function->Prototype() = std::move(PrototypeType);
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_set_default_abi(rp_manager *manager, const char *abi,
                                        rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(abi != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto ABI = model::ABI::fromName(abi);
  if (ABI == model::ABI::Invalid or
      model::ABI::getArchitecture(ABI) != Model->Architecture()) {
    llvmErrorToRpError(revng::createError("invalid ABI for model architecture"),
                       error);
    return false;
  }
  // Preserve the useful meaning of "default": functions that still use the
  // previous default follow it, while explicitly assigned prototypes do not.
  const model::UpcastableType PreviousDefault = Model->DefaultPrototype();
  Model->DefaultABI() = ABI;
  Model->DefaultPrototype() = abi::registerDefaultFunctionPrototype(*Model);
  for (model::Function &Function : Model->Functions())
    if (Function.Prototype() == PreviousDefault)
      Function.Prototype() = Model->DefaultPrototype();
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_set_target_abi(rp_manager *manager, const char *abi,
                                       rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(abi != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto ABI = model::ABI::fromName(abi);
  if (ABI == model::ABI::Invalid or
      model::ABI::getArchitecture(ABI) != Model->Architecture()) {
    llvmErrorToRpError(revng::createError("invalid target ABI for model "
                                          "architecture"),
                       error);
    return false;
  }
  Model->TargetABI() = ABI;
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_set_operating_system(rp_manager *manager,
                                             const char *operating_system,
                                             rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(operating_system != nullptr);
  auto OS = model::OperatingSystem::fromName(operating_system);
  if (OS == model::OperatingSystem::Invalid) {
    llvmErrorToRpError(revng::createError("invalid operating system"), error);
    return false;
  }
  auto &Model = revng::getWritableModelFromContext(manager->context());
  Model->OperatingSystem() = OS;
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_set_platform_name(rp_manager *manager,
                                          const char *platform_name,
                                          rp_error *) {
  revng_check(manager != nullptr);
  revng_check(platform_name != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  Model->PlatformName() = platform_name;
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_set_entry_point(rp_manager *manager,
                                        const char *address, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(address != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  MetaAddress Address = MetaAddress::fromString(address);
  if (Address.isInvalid() or Address.arch() != Model->Architecture()) {
    llvmErrorToRpError(revng::createError("invalid entry point"), error);
    return false;
  }
  Model->EntryPoint() = Address;
  if (not Model->Functions().contains(Address)) {
    model::Function Function;
    Function.Entry() = Address;
    Function.Prototype() = Model->DefaultPrototype();
    Model->Functions().insert(std::move(Function));
  }
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_add_extra_code_address(rp_manager *manager,
                                               const char *address,
                                               rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(address != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  MetaAddress Address = MetaAddress::fromString(address);
  if (Address.isInvalid() or Address.arch() != Model->Architecture()) {
    llvmErrorToRpError(revng::createError("invalid extra code address"), error);
    return false;
  }
  Model->ExtraCodeAddresses().insert(Address);
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_add_function(rp_manager *manager, const char *address,
                                     const char *name, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(address != nullptr);
  revng_check(name != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  MetaAddress Address = MetaAddress::fromString(address);
  if (Address.isInvalid() or Address.arch() != Model->Architecture()) {
    llvmErrorToRpError(revng::createError("invalid function address"), error);
    return false;
  }
  auto Existing = Model->Functions().find(Address);
  if (Existing != Model->Functions().end()) {
    Existing->Name() = name;
  } else {
    model::Function Function;
    Function.Entry() = Address;
    Function.Name() = name;
    Function.Prototype() = Model->DefaultPrototype();
    Model->Functions().insert(std::move(Function));
  }
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_set_function_prototype(rp_manager *manager,
                                               const char *address,
                                               uint64_t type_definition_id,
                                               rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(address != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  MetaAddress Address = MetaAddress::fromString(address);
  auto Function = Model->Functions().find(Address);
  model::TypeDefinition *Definition =
      findTypeDefinition(*Model, type_definition_id);
  if (Function == Model->Functions().end()) {
    llvmErrorToRpError(revng::createError("function address is not known"),
                       error);
    return false;
  }
  if (Definition == nullptr or Definition->getCABIFunction() == nullptr) {
    llvmErrorToRpError(revng::createError("type is not a C ABI function "
                                          "definition"),
                       error);
    return false;
  }
  Function->Prototype() = Model->makeType(Definition->key());
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_add_function_exported_name(rp_manager *manager,
                                                   const char *address,
                                                   const char *name,
                                                   rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(address != nullptr);
  revng_check(name != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto Function = Model->Functions().find(MetaAddress::fromString(address));
  if (Function == Model->Functions().end()) {
    llvmErrorToRpError(revng::createError("function address is not known"),
                       error);
    return false;
  }
  Function->ExportedNames().insert(name);
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_add_imported_library(rp_manager *manager,
                                             const char *name, rp_error *) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  Model->ImportedLibraries().insert(name);
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_add_imported_function(rp_manager *manager,
                                              const char *name,
                                              uint64_t type_definition_id,
                                              rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  model::TypeDefinition *Definition =
      findTypeDefinition(*Model, type_definition_id);
  if (Definition == nullptr or Definition->getCABIFunction() == nullptr) {
    llvmErrorToRpError(revng::createError("import prototype is not a C ABI "
                                          "function definition"),
                       error);
    return false;
  }
  auto &&[Function, Inserted] = Model->ImportedDynamicFunctions().emplace(name);
  if (not Inserted) {
    llvmErrorToRpError(revng::createError("imported function already exists"),
                       error);
    return false;
  }
  Function->Prototype() = Model->makeType(Definition->key());
  manager->recalculateAllPossibleTargets();
  return true;
}

static uint64_t
_rp_manager_create_struct_type(rp_manager *manager, const char *name,
                               const char *comment, uint64_t size,
                               uint64_t can_contain_code, rp_error *) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto &&[Definition, Type] = Model->makeStructDefinition(size);
  Definition.Name() = name;
  Definition.Comment() = comment == nullptr ? "" : comment;
  Definition.CanContainCode() = can_contain_code;
  manager->recalculateAllPossibleTargets();
  return Definition.ID();
}

static bool _rp_manager_add_struct_field(rp_manager *manager,
                                         uint64_t type_definition_id,
                                         uint64_t offset, const char *name,
                                         const char *comment,
                                         const rp_type *type, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  revng_check(type != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto *Definition = llvm::dyn_cast_or_null<model::StructDefinition>(
      findTypeDefinition(*Model, type_definition_id));
  if (Definition == nullptr or Definition->Fields().contains(offset)) {
    llvmErrorToRpError(revng::createError("invalid struct type or duplicate "
                                          "field offset"),
                       error);
    return false;
  }
  auto FieldType = marshalType(*Model, *type, false);
  if (not FieldType) {
    llvmErrorToRpError(FieldType.takeError(), error);
    return false;
  }
  model::StructField &Field =
      Definition->addField(offset, std::move(*FieldType));
  Field.Name() = name;
  Field.Comment() = comment == nullptr ? "" : comment;
  manager->recalculateAllPossibleTargets();
  return true;
}

static uint64_t _rp_manager_create_union_type(rp_manager *manager,
                                              const char *name,
                                              const char *comment, rp_error *) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto &&[Definition, Type] = Model->makeUnionDefinition();
  Definition.Name() = name;
  Definition.Comment() = comment == nullptr ? "" : comment;
  manager->recalculateAllPossibleTargets();
  return Definition.ID();
}

static bool _rp_manager_add_union_field(rp_manager *manager,
                                        uint64_t type_definition_id,
                                        const char *name, const char *comment,
                                        const rp_type *type, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  revng_check(type != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto *Definition = llvm::dyn_cast_or_null<model::UnionDefinition>(
      findTypeDefinition(*Model, type_definition_id));
  if (Definition == nullptr) {
    llvmErrorToRpError(revng::createError("invalid union type"), error);
    return false;
  }
  auto FieldType = marshalType(*Model, *type, false);
  if (not FieldType) {
    llvmErrorToRpError(FieldType.takeError(), error);
    return false;
  }
  model::UnionField &Field = Definition->addField(std::move(*FieldType));
  Field.Name() = name;
  Field.Comment() = comment == nullptr ? "" : comment;
  manager->recalculateAllPossibleTargets();
  return true;
}

static uint64_t _rp_manager_create_enum_type(
    rp_manager *manager, const char *name, const char *comment,
    const rp_primitive_type *underlying_type, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  revng_check(underlying_type != nullptr);
  if (underlying_type->kind != RP_PRIMITIVE_KIND_SIGNED and
      underlying_type->kind != RP_PRIMITIVE_KIND_UNSIGNED) {
    llvmErrorToRpError(revng::createError("enum underlying type must be signed "
                                          "or unsigned"),
                       error);
    return uint64_t(-1);
  }
  auto Underlying = marshalPrimitiveType(*underlying_type, false);
  if (not Underlying) {
    llvmErrorToRpError(Underlying.takeError(), error);
    return uint64_t(-1);
  }
  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto &&[Definition, Type] = Model->makeEnumDefinition();
  Definition.Name() = name;
  Definition.Comment() = comment == nullptr ? "" : comment;
  Definition.UnderlyingType() = std::move(*Underlying);
  manager->recalculateAllPossibleTargets();
  return Definition.ID();
}

static bool _rp_manager_add_enum_entry(rp_manager *manager,
                                       uint64_t type_definition_id,
                                       uint64_t value, const char *name,
                                       const char *comment, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto *Definition = llvm::dyn_cast_or_null<model::EnumDefinition>(
      findTypeDefinition(*Model, type_definition_id));
  if (Definition == nullptr) {
    llvmErrorToRpError(revng::createError("invalid enum type"), error);
    return false;
  }
  auto &&[Entry, Inserted] = Definition->Entries().emplace(value);
  if (not Inserted) {
    llvmErrorToRpError(revng::createError("duplicate enum value"), error);
    return false;
  }
  Entry->Name() = name;
  Entry->Comment() = comment == nullptr ? "" : comment;
  manager->recalculateAllPossibleTargets();
  return true;
}

static uint64_t _rp_manager_create_typedef(rp_manager *manager,
                                           const char *name,
                                           const char *comment,
                                           const rp_type *underlying_type,
                                           rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  revng_check(underlying_type != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto Underlying = marshalType(*Model, *underlying_type, false);
  if (not Underlying) {
    llvmErrorToRpError(Underlying.takeError(), error);
    return uint64_t(-1);
  }
  auto &&[Definition, Type] =
      Model->makeTypedefDefinition(std::move(*Underlying));
  Definition.Name() = name;
  Definition.Comment() = comment == nullptr ? "" : comment;
  manager->recalculateAllPossibleTargets();
  return Definition.ID();
}

static uint64_t _rp_manager_create_cabi_type(
    rp_manager *manager, const char *name, const char *comment, const char *abi,
    uint64_t arguments_count, const rp_typed_argument arguments[],
    const rp_type *return_type, const char *return_value_comment,
    rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  revng_check(abi != nullptr);
  if (arguments_count != 0 and arguments == nullptr) {
    llvmErrorToRpError(revng::createError("null function argument array"),
                       error);
    return uint64_t(-1);
  }
  auto &Model = revng::getWritableModelFromContext(manager->context());
  auto ABI = model::ABI::fromName(abi);
  if (ABI == model::ABI::Invalid or
      model::ABI::getArchitecture(ABI) != Model->Architecture()) {
    llvmErrorToRpError(revng::createError("invalid function ABI"), error);
    return uint64_t(-1);
  }
  std::vector<model::UpcastableType> ArgumentTypes;
  ArgumentTypes.reserve(arguments_count);
  for (uint64_t I = 0; I < arguments_count; ++I) {
    if (arguments[I].name == nullptr) {
      llvmErrorToRpError(revng::createError("null function argument name"),
                         error);
      return uint64_t(-1);
    }
    auto Type = marshalType(*Model, arguments[I].type, false);
    if (not Type) {
      llvmErrorToRpError(Type.takeError(), error);
      return uint64_t(-1);
    }
    ArgumentTypes.push_back(std::move(*Type));
  }
  std::optional<model::UpcastableType> ReturnType;
  if (return_type != nullptr and
      not(return_type->kind == RP_TYPE_KIND_PRIMITIVE and
          return_type->primitive.kind == RP_PRIMITIVE_KIND_VOID)) {
    auto Type = marshalType(*Model, *return_type, false);
    if (not Type) {
      llvmErrorToRpError(Type.takeError(), error);
      return uint64_t(-1);
    }
    ReturnType.emplace(std::move(*Type));
  }
  auto &&[Definition, Type] = Model->makeCABIFunctionDefinition();
  Definition.Name() = name;
  Definition.Comment() = comment == nullptr ? "" : comment;
  Definition.ABI() = ABI;
  for (uint64_t I = 0; I < arguments_count; ++I) {
    auto &Argument = Definition.addArgument(std::move(ArgumentTypes[I]));
    Argument.Name() = arguments[I].name;
    Argument.Comment() =
        arguments[I].comment == nullptr ? "" : arguments[I].comment;
  }
  if (ReturnType)
    Definition.ReturnType() = std::move(*ReturnType);
  Definition.ReturnValueComment() =
      return_value_comment == nullptr ? "" : return_value_comment;
  manager->recalculateAllPossibleTargets();
  return Definition.ID();
}

static uint64_t _rp_manager_create_raw_function_type(
    rp_manager *manager, const char *name, const char *comment,
    const char *architecture, uint64_t arguments_count,
    const rp_named_typed_register arguments[], uint64_t return_values_count,
    const rp_named_typed_register return_values[],
    uint64_t preserved_registers_count, const char *preserved_registers[],
    uint64_t final_stack_offset, const rp_type *stack_arguments_type,
    const char *return_value_comment, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  revng_check(architecture != nullptr);
  if ((arguments_count != 0 and arguments == nullptr) or
      (return_values_count != 0 and return_values == nullptr) or
      (preserved_registers_count != 0 and preserved_registers == nullptr)) {
    llvmErrorToRpError(revng::createError("null raw function array"), error);
    return uint64_t(-1);
  }

  auto &Model = revng::getWritableModelFromContext(manager->context());
  const model::Architecture::Values Architecture =
      model::Architecture::fromName(architecture);
  if (Architecture == model::Architecture::Invalid) {
    llvmErrorToRpError(revng::createError("invalid raw function architecture"),
                       error);
    return uint64_t(-1);
  }

  struct ParsedRegister {
    model::Register::Values Location;
    std::string Name;
    std::string Comment;
    model::UpcastableType Type;
  };
  auto ParseRegisters = [&](uint64_t Count,
                            const rp_named_typed_register Input[])
      -> llvm::Expected<std::vector<ParsedRegister>> {
    std::vector<ParsedRegister> Result;
    std::set<model::Register::Values> Seen;
    Result.reserve(Count);
    for (uint64_t I = 0; I < Count; ++I) {
      if (Input[I].register_name == nullptr or Input[I].name == nullptr)
        return revng::createError("null raw function register or name");
      const model::Register::Values Register =
          model::Register::fromRegisterName(Input[I].register_name,
                                            Architecture);
      if (not model::Register::isValid(Register) or
          not model::Register::isUsedInArchitecture(Register, Architecture))
        return revng::createError("invalid register for raw function "
                                  "architecture");
      if (not Seen.insert(Register).second)
        return revng::createError("duplicate raw function register");
      auto Type = marshalType(*Model, Input[I].type, false);
      if (not Type)
        return Type.takeError();
      if (not(*Type)->isScalar())
        return revng::createError("raw function register types must be scalar");
      model::NamedTypedRegister Candidate(Register);
      Candidate.Type() = (*Type).copy();
      if (not Candidate.verify(false))
        return revng::createError(
            "raw function type does not fit its register");
      Result.push_back({Register, Input[I].name,
                        Input[I].comment == nullptr ? "" : Input[I].comment,
                        std::move(*Type)});
    }
    return Result;
  };

  auto Arguments = ParseRegisters(arguments_count, arguments);
  if (not Arguments) {
    llvmErrorToRpError(Arguments.takeError(), error);
    return uint64_t(-1);
  }
  auto ReturnValues = ParseRegisters(return_values_count, return_values);
  if (not ReturnValues) {
    llvmErrorToRpError(ReturnValues.takeError(), error);
    return uint64_t(-1);
  }

  std::vector<model::Register::Values> PreservedRegisters;
  std::set<model::Register::Values> SeenPreserved;
  PreservedRegisters.reserve(preserved_registers_count);
  for (uint64_t I = 0; I < preserved_registers_count; ++I) {
    if (preserved_registers[I] == nullptr) {
      llvmErrorToRpError(revng::createError("null preserved register"), error);
      return uint64_t(-1);
    }
    const model::Register::Values Register =
        model::Register::fromRegisterName(preserved_registers[I], Architecture);
    if (not model::Register::isValid(Register) or
        not model::Register::isUsedInArchitecture(Register, Architecture) or
        not SeenPreserved.insert(Register).second) {
      llvmErrorToRpError(revng::createError("invalid or duplicate preserved "
                                            "register"),
                         error);
      return uint64_t(-1);
    }
    PreservedRegisters.push_back(Register);
  }

  std::optional<model::UpcastableType> StackArgumentsType;
  if (stack_arguments_type != nullptr) {
    auto Type = marshalType(*Model, *stack_arguments_type, false);
    if (not Type) {
      llvmErrorToRpError(Type.takeError(), error);
      return uint64_t(-1);
    }
    if ((*Type)->getStruct() == nullptr) {
      llvmErrorToRpError(revng::createError("raw function stack arguments must "
                                            "be a struct type"),
                         error);
      return uint64_t(-1);
    }
    StackArgumentsType.emplace(std::move(*Type));
  }

  auto &&[Definition, Type] = Model->makeRawFunctionDefinition();
  Definition.Name() = name;
  Definition.Comment() = comment == nullptr ? "" : comment;
  Definition.Architecture() = Architecture;
  for (ParsedRegister &Input : *Arguments) {
    model::NamedTypedRegister &Argument =
        Definition.addArgument(Input.Location, std::move(Input.Type));
    Argument.Name() = std::move(Input.Name);
    Argument.Comment() = std::move(Input.Comment);
  }
  for (ParsedRegister &Input : *ReturnValues) {
    model::NamedTypedRegister &ReturnValue =
        Definition.addReturnValue(Input.Location, std::move(Input.Type));
    ReturnValue.Name() = std::move(Input.Name);
    ReturnValue.Comment() = std::move(Input.Comment);
  }
  for (model::Register::Values Register : PreservedRegisters)
    Definition.PreservedRegisters().insert(Register);
  Definition.FinalStackOffset() = final_stack_offset;
  if (StackArgumentsType)
    Definition.StackArgumentsType() = std::move(*StackArgumentsType);
  Definition.ReturnValueComment() =
      return_value_comment == nullptr ? "" : return_value_comment;
  manager->recalculateAllPossibleTargets();
  return Definition.ID();
}

static bool _rp_manager_add_data_symbol(rp_manager *manager,
                                        const char *address, const char *name,
                                        const rp_type *type, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(address != nullptr);
  revng_check(name != nullptr);
  revng_check(type != nullptr);
  auto &Model = revng::getWritableModelFromContext(manager->context());
  MetaAddress Address = MetaAddress::fromString(address);
  model::Segment *Match = nullptr;
  for (model::Segment &Segment : Model->Segments())
    if (Segment.contains(Address)) {
      if (Match != nullptr) {
        llvmErrorToRpError(revng::createError("data symbol address is "
                                              "ambiguous"),
                           error);
        return false;
      }
      Match = &Segment;
    }
  if (Match == nullptr) {
    llvmErrorToRpError(revng::createError("data symbol is outside all "
                                          "segments"),
                       error);
    return false;
  }
  auto SymbolType = marshalType(*Model, *type, false);
  if (not SymbolType) {
    llvmErrorToRpError(SymbolType.takeError(), error);
    return false;
  }
  model::StructDefinition *SegmentType = Match->type();
  if (SegmentType == nullptr) {
    auto &&[Definition, DefinedType] =
        Model->makeStructDefinition(Match->VirtualSize());
    Definition.Name() = Match->Name().empty() ? "segment" : Match->Name();
    Match->Type() = std::move(DefinedType);
    SegmentType = &Definition;
  }
  const uint64_t Offset = Address.address() - Match->StartAddress().address();
  if (SegmentType->Fields().contains(Offset)) {
    llvmErrorToRpError(revng::createError("duplicate data symbol address"),
                       error);
    return false;
  }
  SegmentType->addField(Offset, std::move(*SymbolType)).Name() = name;
  manager->recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_save(rp_manager *manager) {
  revng_check(manager != nullptr);

  const auto &Model = revng::getModelFromContext(manager->context());
  if (findRawBinaryProvider(*Model))
    return false;

  auto Error = manager->store();
  if (not Error)
    return true;

  llvm::consumeError(std::move(Error));
  return false;
}

static void _rp_manager_destroy(rp_manager *manager) {
  revng_check(manager != nullptr);
  const auto &Model = revng::getModelFromContext(manager->context());
  revng::lift::LifterRegistry::clearOverride(*Model);
  unregisterRawBinaryProvider(*Model);
  delete manager;
}

static rp_step *_rp_manager_get_step_from_name(rp_manager *manager,
                                               const char *name) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);
  return &manager->getRunner().getStep(name);
}

static rp_container *
_rp_step_get_container(rp_step *step, rp_container_identifier *container) {
  revng_check(step != nullptr);
  revng_check(container != nullptr);

  if (step->containers().isContainerRegistered(container->first())) {
    step->containers()[container->first()];
    return &*step->containers().find(container->first());
  } else {
    return nullptr;
  }
}

static uint64_t
_rp_targets_list_targets_count(const rp_targets_list *targets_list) {
  revng_check(targets_list != nullptr);
  return targets_list->size();
}

static rp_kind *_rp_manager_get_kind_from_name(const rp_manager *manager,
                                               const char *kind_name) {
  revng_check(manager != nullptr);
  revng_check(kind_name != nullptr);

  return manager->getKind(kind_name);
}

static rp_diff_map *_rp_manager_run_analysis(
    rp_manager *manager, const char *step_name, const char *analysis_name,
    const rp_container_targets_map *target_map, const rp_string_map *options,
    rp_invalidations *invalidations, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(step_name != nullptr);
  revng_check(analysis_name != nullptr);
  revng_check(target_map != nullptr);

  ExistingOrNew<rp_invalidations> Invalidations(invalidations);
  ExistingOrNew<const rp_string_map> Options(options);

  auto MaybeDiffs = manager->runAnalysis(analysis_name, step_name, *target_map,
                                         *Invalidations, *Options);
  if (not MaybeDiffs) {
    llvmErrorToRpError(MaybeDiffs.takeError(), error);
    return nullptr;
  }

  return new rp_diff_map(std::move(*MaybeDiffs));
}

static void _rp_diff_map_destroy(rp_diff_map *map) {
  revng_check(map != nullptr);
  delete map;
}

static char *_rp_diff_map_get_diff(const rp_diff_map *map,
                                   const char *global_name) {
  revng_check(map != nullptr);
  revng_check(global_name != nullptr);

  auto It = map->find(global_name);
  if (It == map->end())
    return nullptr;

  std::string S;
  llvm::raw_string_ostream OS(S);
  It->second.serialize(OS);
  OS.flush();
  return copyString(S);
}

static rp_buffer *_rp_manager_produce_targets(
    rp_manager *manager, const rp_step *step, const rp_container *container,
    uint64_t targets_count, rp_target *targets[], rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(step != nullptr);
  revng_check(container != nullptr);
  revng_check(targets_count != 0);
  revng_check(targets != nullptr);

  TargetsList List;
  for (size_t I = 0; I < targets_count; I++)
    List.push_back(*targets[I]);

  auto ErrorOrCloned =
      manager->produceTargets(step->getName(), *container, List);

  if (!ErrorOrCloned) {
    llvmErrorToRpError(ErrorOrCloned.takeError(), error);
    return nullptr;
  }

  auto &Cloned = ErrorOrCloned.get();
  rp_buffer *Out = new rp_buffer();
  llvm::raw_svector_ostream Serialized(*Out);
  llvm::cantFail(Cloned->serialize(Serialized));

  return Out;
}

static rp_target *_rp_target_create(const rp_kind *kind,
                                    uint64_t path_components_count,
                                    const char *path_components[]) {
  revng_check(kind != nullptr);
  revng_check(path_components != nullptr);
  revng_check(kind->rank().depth() == path_components_count);
  Target::PathComponents List;
  for (size_t I = 0; I < path_components_count; I++) {
    llvm::StringRef Rank(path_components[I]);
    List.push_back(Rank.str());
  }

  return new Target(std::move(List), *kind);
}

static void _rp_target_destroy(rp_target *target) {
  revng_check(target != nullptr);
  delete target;
}

static bool _rp_manager_container_deserialize(
    rp_manager *manager, rp_step *step, const char *container_name,
    const char *content, uint64_t size, rp_invalidations *invalidations) {
  revng_check(manager != nullptr);
  revng_check(step != nullptr);
  revng_check(container_name != nullptr);
  revng_check(content != nullptr);

  llvm::StringRef String(content, size);
  auto Buffer = llvm::MemoryBuffer::getMemBuffer(String, "", false);
  auto MaybeInvalidations =
      manager->deserializeContainer(*step, container_name, *Buffer);
  if (not MaybeInvalidations) {
    llvm::consumeError(MaybeInvalidations.takeError());
    return false;
  }

  ExistingOrNew<rp_invalidations> Invalidations(invalidations);
  *Invalidations = MaybeInvalidations.get();
  return true;
}

static const char *_rp_target_get_kind(rp_target *target) {
  revng_check(target != nullptr);
  return target->getKind().name().data();
}

static uint64_t _rp_target_path_components_count(rp_target *target) {
  revng_check(target != nullptr);
  return target->getPathComponents().size();
}

static const char *_rp_target_get_path_component(rp_target *target,
                                                 uint64_t index) {
  revng_check(target != nullptr);
  auto &PathComponents = target->getPathComponents();
  revng_check(index < PathComponents.size());

  return PathComponents[index].c_str();
}

static rp_targets_list *
_rp_manager_get_container_targets_list(const rp_manager *manager,
                                       const rp_container *container) {
  revng_check(manager != nullptr);
  revng_check(container != nullptr);

  return manager->getTargetsAvailableFor(*container);
}

static rp_target *
_rp_targets_list_get_target(const rp_targets_list *targets_list,
                            uint64_t index) {
  revng_check(targets_list != nullptr);
  revng_check(index < targets_list->size());
  return new rp_target((*targets_list)[index]);
}

static void _rp_string_destroy(char *string) {
  revng_check(string != nullptr);
  free(string);
}

static const rp_container_identifier *
_rp_manager_get_container_identifier_from_name(const rp_manager *manager,
                                               const char *name) {
  revng_check(manager != nullptr);
  revng_check(name != nullptr);

  const auto &ContainerRegistry = manager->getRunner().getContainerFactorySet();
  return &ContainerRegistry.at(name);
}

static char *_rp_target_create_serialized_string(rp_target *target) {
  revng_check(target != nullptr);
  return copyString(target->toString());
}

static bool _rp_target_is_ready(const rp_target *target,
                                const rp_container *container) {
  revng_assert(target);
  revng_assert(container);
  return container->second->enumerate().contains(*target);
}

// TODO Remove the redundant copy by writing a custom string stream that writes
// directly to a buffer to return.
static char *_rp_manager_create_global_copy(const rp_manager *manager,
                                            const char *global_name) {
  revng_check(manager != nullptr);
  revng_check(global_name != nullptr);

  std::string Out;
  llvm::raw_string_ostream Serialized(Out);
  auto &GlobalsMap = manager->context().getGlobals();
  if (auto Error = GlobalsMap.serialize(global_name, Serialized)) {
    llvm::consumeError(std::move(Error));
    return nullptr;
  }
  Serialized.flush();
  return copyString(Out);
}

static const char *_rp_container_get_mime(const rp_container *container) {
  revng_check(container != nullptr);
  return container->getValue()->mimeType().data();
}

static rp_buffer *_rp_container_extract_one(const rp_container *container,
                                            const rp_target *target) {
  revng_check(container != nullptr);
  revng_check(target != nullptr);

  if (!container->second->enumerate().contains(*target)) {
    return nullptr;
  }

  rp_buffer *Out = new rp_buffer();
  llvm::raw_svector_ostream Serialized(*Out);
  llvm::cantFail(container->second->extractOne(Serialized, *target));

  return Out;
}

static rp_diff_map *_rp_manager_run_analyses_list(
    rp_manager *manager, const char *list_name, const rp_string_map *options,
    rp_invalidations *invalidations, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(list_name != nullptr);

  ExistingOrNew<rp_invalidations> Invalidations(invalidations);
  ExistingOrNew<const rp_string_map> Options(options);

  const AnalysesList &AL = manager->getRunner().getAnalysesList(list_name);
  auto MaybeDiffs = manager->runAnalyses(AL, *Invalidations, *Options);
  if (not MaybeDiffs) {
    llvmErrorToRpError(MaybeDiffs.takeError(), error);
    return nullptr;
  }

  return new rp_diff_map(std::move(*MaybeDiffs));
}

static bool _rp_diff_map_is_empty(rp_diff_map *map) {
  revng_check(map != nullptr);
  for (auto &Entry : *map) {
    if (!Entry.second.isEmpty()) {
      return false;
    }
  }
  return true;
}

static rp_error *_rp_error_create() { return new rp_error(); }

static bool _rp_error_is_success(const rp_error *error) {
  revng_check(error != nullptr);
  return std::holds_alternative<std::monostate>(*error);
}

static bool _rp_error_is_document_error(const rp_error *error) {
  revng_check(error != nullptr);
  return std::holds_alternative<rp_document_error>(*error);
}

static rp_simple_error *_rp_error_get_simple_error(rp_error *error) {
  revng_check(error != nullptr);
  if (not std::holds_alternative<rp_simple_error>(*error))
    return nullptr;

  return &std::get<rp_simple_error>(*error);
}

static rp_document_error *_rp_error_get_document_error(rp_error *error) {
  revng_check(error != nullptr);
  if (not std::holds_alternative<rp_document_error>(*error))
    return nullptr;

  return &std::get<rp_document_error>(*error);
}

static void _rp_error_destroy(rp_error *error) {
  revng_check(error != nullptr);
  delete error;
}

static size_t _rp_document_error_reasons_count(const rp_document_error *error) {
  revng_check(error != nullptr);
  return error->Reasons.size();
}

static const char *
_rp_document_error_get_error_type(const rp_document_error *error) {
  revng_check(error != nullptr);
  return error->ErrorType.c_str();
}

static const char *
_rp_document_error_get_location_type(const rp_document_error *error) {
  revng_check(error != nullptr);
  return error->LocationType.c_str();
}

static const char *
_rp_document_error_get_error_message(const rp_document_error *error,
                                     uint64_t index) {
  revng_check(error != nullptr);
  return error->Reasons.at(index).Message.c_str();
}

static const char *
_rp_document_error_get_error_location(const rp_document_error *error,
                                      uint64_t index) {
  revng_check(error != nullptr);
  return error->Reasons.at(index).Location.c_str();
}

static const char *
_rp_simple_error_get_error_type(const rp_simple_error *error) {
  revng_check(error != nullptr);
  return error->ErrorType.c_str();
}

static const char *_rp_simple_error_get_message(const rp_simple_error *error) {
  revng_check(error != nullptr);
  return error->Message.c_str();
}

static rp_string_map *_rp_string_map_create() { return new rp_string_map(); }

static void _rp_string_map_destroy(rp_string_map *map) {
  revng_check(map != nullptr);
  delete map;
}

static void _rp_string_map_insert(rp_string_map *map, const char *key,
                                  const char *value) {
  revng_check(map != nullptr);
  revng_check(key != nullptr);
  revng_check(value != nullptr);

  auto Result = map->insert_or_assign(key, value);
  revng_assert(Result.second);
}

static rp_invalidations *_rp_invalidations_create() {
  return new rp_invalidations();
}

static void _rp_invalidations_destroy(rp_invalidations *invalidations) {
  revng_check(invalidations != nullptr);
  delete invalidations;
}

static char *
_rp_invalidations_serialize(const rp_invalidations *invalidations) {
  revng_check(invalidations != nullptr);
  std::string Out;

  for (auto &StepPair : *invalidations) {
    for (auto &ContainerPair : StepPair.second) {
      for (auto Target : ContainerPair.second) {
        Out += llvm::join_items('/', StepPair.first(), ContainerPair.first(),
                                Target.toString()) +
               '\n';
      }
    }
  }

  return copyString(Out);
}

static uint64_t _rp_buffer_size(const rp_buffer *buffer) {
  revng_check(buffer != nullptr);
  return buffer->size();
}

static const char *_rp_buffer_data(const rp_buffer *buffer) {
  revng_check(buffer != nullptr);
  return buffer->data();
}

static void _rp_buffer_destroy(rp_buffer *buffer) {
  revng_check(buffer != nullptr);
  delete buffer;
}

static rp_container_targets_map *_rp_container_targets_map_create() {
  return new ContainerToTargetsMap();
}

static void _rp_container_targets_map_destroy(rp_container_targets_map *map) {
  revng_check(map != nullptr);
  delete map;
}

static void _rp_container_targets_map_add(rp_container_targets_map *map,
                                          const rp_container *container,
                                          const rp_target *target) {
  revng_check(map != nullptr);
  revng_check(container != nullptr);
  revng_check(target != nullptr);
  (*map)[container->second->name()].push_back(*target);
}

static const char *_rp_manager_get_pipeline_description(rp_manager *manager) {
  revng_check(manager != nullptr);
  return manager->getPipelineDescription().data();
}

static uint64_t _rp_manager_get_context_commit_index(rp_manager *manager) {
  revng_check(manager != nullptr);
  return manager->context().getCommitIndex();
}

static pipeline::Step *findStep(rp_manager &Manager, llvm::StringRef Name) {
  for (pipeline::Step &Step : Manager.getRunner())
    if (Step.getName() == Name)
      return &Step;
  return nullptr;
}

static rp_container *findContainer(pipeline::Step &Step, llvm::StringRef Name) {
  if (not Step.containers().isContainerRegistered(Name))
    return nullptr;
  Step.containers()[Name];
  auto Iterator = Step.containers().find(Name);
  return Iterator == Step.containers().end() ? nullptr : &*Iterator;
}

static std::unique_ptr<rp_buffer>
produceArtifact(rp_manager &Manager, llvm::StringRef StepName,
                llvm::StringRef ContainerName, llvm::StringRef KindName,
                llvm::ArrayRef<const char *> PathComponents, rp_error *Error) {
  pipeline::Step *Step = findStep(Manager, StepName);
  if (Step == nullptr) {
    llvmErrorToRpError(revng::createError("unknown pipeline step: " + StepName),
                       Error);
    return nullptr;
  }
  rp_container *Container = findContainer(*Step, ContainerName);
  if (Container == nullptr or Container->second == nullptr) {
    llvmErrorToRpError(
        revng::createError("unknown pipeline container: " + ContainerName),
        Error);
    return nullptr;
  }
  const pipeline::Kind *Kind = Manager.getKind(KindName);
  if (Kind == nullptr) {
    llvmErrorToRpError(revng::createError("unknown pipeline kind: " + KindName),
                       Error);
    return nullptr;
  }
  if (Kind->rank().depth() != PathComponents.size()) {
    llvmErrorToRpError(revng::createError("artifact target path has the wrong "
                                          "number of components"),
                       Error);
    return nullptr;
  }
  pipeline::Target::PathComponents Path;
  Path.reserve(PathComponents.size());
  for (const char *Component : PathComponents) {
    if (Component == nullptr) {
      llvmErrorToRpError(revng::createError("null artifact path component"),
                         Error);
      return nullptr;
    }
    Path.emplace_back(Component);
  }
  pipeline::Target Target(std::move(Path), *Kind);
  pipeline::TargetsList Targets({Target});
  auto Produced = Manager.produceTargets(StepName, *Container, Targets);
  if (not Produced) {
    llvmErrorToRpError(Produced.takeError(), Error);
    return nullptr;
  }
  auto Result = std::make_unique<rp_buffer>();
  llvm::raw_svector_ostream Stream(*Result);
  if (llvm::Error ExtractError = (*Produced)->extractOne(Stream, Target)) {
    llvmErrorToRpError(std::move(ExtractError), Error);
    return nullptr;
  }
  return Result;
}

static rp_buffer *
_rp_manager_produce_artifact(rp_manager *manager, const char *step_name,
                             const char *container_name, const char *kind_name,
                             uint64_t path_components_count,
                             const char *path_components[], rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(step_name != nullptr);
  revng_check(container_name != nullptr);
  revng_check(kind_name != nullptr);
  if (path_components_count != 0 and path_components == nullptr) {
    llvmErrorToRpError(revng::createError("null artifact target path"), error);
    return nullptr;
  }
  return produceArtifact(*manager, step_name, container_name, kind_name,
                         {path_components, size_t(path_components_count)},
                         error)
      .release();
}

static llvm::Error invalidateDownstream(rp_manager &Manager,
                                        pipeline::Step &Step,
                                        const rp_container &Container,
                                        const pipeline::TargetsList &Targets) {
  pipeline::ContainerToTargetsMap Seed;
  Seed.add(Container.first(), Targets);

  // A module can be an intermediate container within a step (functions.mlir
  // in emit-c is the important case), so first propagate invalidations through
  // the remaining pipes in the current step. Runner::getInvalidations then
  // carries those results to successor steps.
  pipeline::ContainerToTargetsMap SameStep = Step.deduceResults(Seed, true);
  Step.containers().intersect(SameStep);
  pipeline::TargetInStepSet Invalidations;
  Invalidations[Step.getName()].merge(SameStep);
  Manager.getRunner().getInvalidations(Invalidations);

  // Keep the just-committed module while dropping all artifacts derived from
  // its previous contents, including outputs produced later in this step.
  Invalidations[Step.getName()].erase(Container.first());
  return Manager.getRunner().invalidate(Invalidations);
}

template <typename ContainerT, typename CallbackT>
static bool transformModule(rp_manager &Manager, llvm::StringRef StepName,
                            llvm::StringRef ContainerName,
                            const CallbackT &Callback, rp_error *Error) {
  pipeline::Step *Step = findStep(Manager, StepName);
  if (Step == nullptr) {
    llvmErrorToRpError(revng::createError("unknown pipeline step: " + StepName),
                       Error);
    return false;
  }
  rp_container *Container = findContainer(*Step, ContainerName);
  if (Container == nullptr or Container->second == nullptr or
      not llvm::isa<ContainerT>(Container->second.get())) {
    llvmErrorToRpError(revng::createError("pipeline container has the wrong "
                                          "module type"),
                       Error);
    return false;
  }
  pipeline::TargetsList Targets = Container->second->enumerate();
  if (Targets.empty()) {
    llvmErrorToRpError(revng::createError("module container has no produced "
                                          "targets"),
                       Error);
    return false;
  }
  std::unique_ptr<pipeline::ContainerBase> Clone =
      Container->second->cloneFiltered(Targets);
  auto &TypedClone = llvm::cast<ContainerT>(*Clone);
  const char *CallbackError = nullptr;
  if (not Callback(TypedClone, &CallbackError)) {
    llvmErrorToRpError(
        revng::createError(CallbackError == nullptr
                               ? "module transform callback failed"
                               : CallbackError),
        Error);
    return false;
  }

  llvm::SmallVector<char, 0> Serialized;
  llvm::raw_svector_ostream Stream(Serialized);
  if (llvm::Error SerializeError = Clone->serialize(Stream)) {
    llvmErrorToRpError(std::move(SerializeError), Error);
    return false;
  }
  llvm::MemoryBufferRef Ref({Serialized.data(), Serialized.size()},
                            "transformed-module");
  if (llvm::Error DeserializeError = Container->second->deserialize(
          *llvm::MemoryBuffer::getMemBuffer(Ref, false))) {
    llvmErrorToRpError(std::move(DeserializeError), Error);
    return false;
  }
  if (llvm::Error InvalidationError =
          invalidateDownstream(Manager, *Step, *Container, Targets)) {
    llvmErrorToRpError(std::move(InvalidationError), Error);
    return false;
  }
  Manager.recalculateAllPossibleTargets();
  return true;
}

static bool _rp_manager_transform_llvm_module(
    rp_manager *manager, const char *step_name, const char *container_name,
    const rp_llvm_module_callbacks *callbacks, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(step_name != nullptr);
  revng_check(container_name != nullptr);
  if (callbacks == nullptr or callbacks->transform == nullptr) {
    llvmErrorToRpError(revng::createError("incomplete LLVM transform callback"),
                       error);
    return false;
  }
  return transformModule<pipeline::LLVMContainer>(
      *manager, step_name, container_name,
      [&](pipeline::LLVMContainer &Container, const char **CallbackError) {
        bool Success = callbacks->transform(callbacks->opaque,
                                            llvm::wrap(&Container.getModule()),
                                            CallbackError);
        if (Success) {
          std::string VerificationError;
          llvm::raw_string_ostream VerificationStream(VerificationError);
          if (llvm::verifyModule(Container.getModule(), &VerificationStream)) {
            VerificationStream.flush();
            if (*CallbackError == nullptr) {
              static thread_local std::string ErrorStorage;
              ErrorStorage = std::move(VerificationError);
              *CallbackError = ErrorStorage.c_str();
            }
            return false;
          }
        }
        return Success;
      },
      error);
}

static bool transformMLIRContainer(const rp_mlir_module_callbacks &Callbacks,
                                   pipeline::ContainerBase &Container,
                                   const char **CallbackError) {
  const void *Handle = Container.getMLIRModuleHandle();
  if (Handle == nullptr) {
    *CallbackError = "pipeline container is not an MLIR module container";
    return false;
  }
  mlir::ModuleOp Module =
      mlir::ModuleOp::getFromOpaquePointer(const_cast<void *>(Handle));
  bool Success = Callbacks.transform(Callbacks.opaque, {Handle}, CallbackError);
  if (Success and failed(mlir::verify(Module))) {
    if (*CallbackError == nullptr)
      *CallbackError = "MLIR module verification failed";
    return false;
  }
  return Success;
}

static bool _rp_manager_transform_mlir_module(
    rp_manager *manager, const char *step_name, const char *container_name,
    const rp_mlir_module_callbacks *callbacks, rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(step_name != nullptr);
  revng_check(container_name != nullptr);
  if (callbacks == nullptr or callbacks->transform == nullptr) {
    llvmErrorToRpError(revng::createError("incomplete MLIR transform callback"),
                       error);
    return false;
  }
  return transformModule<pipeline::ContainerBase>(
      *manager, step_name, container_name,
      [&](pipeline::ContainerBase &Value, const char **CallbackError) {
        return transformMLIRContainer(*callbacks, Value, CallbackError);
      },
      error);
}

static rp_buffer *decompilePTML(rp_manager *Manager, const char *Address,
                                rp_error *Error) {
  const bool SingleFunction = Address != nullptr;
  const char *StepName = SingleFunction ? "emit-c" : "emit-c-as-single-file";
  const char *ContainerName =
      SingleFunction ? "decompiled.tar.gz" : "decompiled.c";
  const char *KindName = SingleFunction ? "decompiled" : "decompiled-to-c";
  pipeline::Step &Step = Manager->getRunner().getStep(StepName);
  const auto &Factories = Manager->getRunner().getContainerFactorySet();
  const auto &Factory = Factories.at(ContainerName);
  const pipeline::Kind *Kind = Manager->getKind(KindName);
  if (Kind == nullptr) {
    llvmErrorToRpError(revng::createError("C decompilation artifact is not "
                                          "available in this pipeline"),
                       Error);
    return nullptr;
  }
  Step.containers()[Factory.first()];
  auto Container = Step.containers().find(Factory.first());
  if (Container == Step.containers().end()) {
    llvmErrorToRpError(revng::createError("C decompilation container is not "
                                          "available"),
                       Error);
    return nullptr;
  }
  pipeline::Target::PathComponents Path;
  if (SingleFunction) {
    MetaAddress FunctionAddress = MetaAddress::fromString(Address);
    const auto &Model = revng::getModelFromContext(Manager->context());
    if (FunctionAddress.isInvalid() or
        not Model->Functions().contains(FunctionAddress)) {
      llvmErrorToRpError(revng::createError("function address is not known"),
                         Error);
      return nullptr;
    }
    Path.push_back(FunctionAddress.toString());
  }
  pipeline::Target Target(std::move(Path), *Kind);
  pipeline::TargetsList Targets;
  Targets.push_back(Target);
  auto Produced = Manager->produceTargets(StepName, *Container, Targets);
  if (not Produced) {
    llvmErrorToRpError(Produced.takeError(), Error);
    return nullptr;
  }
  auto Result = std::make_unique<rp_buffer>();
  llvm::raw_svector_ostream Stream(*Result);
  if (llvm::Error ExtractError = (*Produced)->extractOne(Stream, Target)) {
    llvmErrorToRpError(std::move(ExtractError), Error);
    return nullptr;
  }
  return Result.release();
}

static std::unique_ptr<rp_buffer> stripPTML(const rp_buffer &Input) {
  auto Result = std::make_unique<rp_buffer>();
  bool InTag = false;
  char Quote = 0;
  for (size_t I = 0; I < Input.size();) {
    const char C = Input[I];
    if (InTag) {
      if (Quote != 0) {
        if (C == Quote)
          Quote = 0;
      } else if (C == '\'' or C == '"') {
        Quote = C;
      } else if (C == '>') {
        InTag = false;
      }
      ++I;
      continue;
    }
    if (C == '<') {
      InTag = true;
      ++I;
      continue;
    }
    if (C == '&') {
      llvm::StringRef Remaining(Input.data() + I, Input.size() - I);
      struct Entity {
        llvm::StringLiteral Encoded;
        char Decoded;
      };
      static constexpr Entity Entities[] = {{"&amp;", '&'},
                                            {"&lt;", '<'},
                                            {"&gt;", '>'},
                                            {"&quot;", '"'},
                                            {"&apos;", '\''}};
      bool Decoded = false;
      for (const Entity &Entity : Entities) {
        if (Remaining.starts_with(Entity.Encoded)) {
          Result->push_back(Entity.Decoded);
          I += Entity.Encoded.size();
          Decoded = true;
          break;
        }
      }
      if (Decoded)
        continue;
    }
    Result->push_back(C);
    ++I;
  }
  return Result;
}

static rp_buffer *_rp_manager_decompile_to_ptml(rp_manager *manager,
                                                rp_error *error) {
  revng_check(manager != nullptr);
  return decompilePTML(manager, nullptr, error);
}

static rp_buffer *_rp_manager_decompile_to_c(rp_manager *manager,
                                             rp_error *error) {
  revng_check(manager != nullptr);
  std::unique_ptr<rp_buffer> PTML(decompilePTML(manager, nullptr, error));
  return PTML ? stripPTML(*PTML).release() : nullptr;
}

static rp_buffer *_rp_manager_decompile_to_c_bundle(rp_manager *manager,
                                                    rp_error *error) {
  revng_check(manager != nullptr);
  std::unique_ptr<rp_buffer> Functions(
      _rp_manager_decompile_to_c(manager, error));
  if (not Functions)
    return nullptr;
  std::unique_ptr<rp_buffer> TypesPTML = produceArtifact(
      *manager, "emit-type-and-global-header", "types-and-globals.h",
      "type-and-global-header", {}, error);
  if (not TypesPTML)
    return nullptr;
  std::unique_ptr<rp_buffer> HelpersPTML = produceArtifact(
      *manager, "emit-helper-header", "helpers.h", "helper-header", {}, error);
  if (not HelpersPTML)
    return nullptr;
  std::unique_ptr<rp_buffer> Types = stripPTML(*TypesPTML);
  std::unique_ptr<rp_buffer> Helpers = stripPTML(*HelpersPTML);

  auto ReadResource = [&](llvm::StringRef Name)
      -> llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> {
    std::string ResourceName = ("share/revng/include/" + Name).str();
    auto Path = revng::ResourceFinder.findFile(ResourceName);
    if (not Path or Path->empty())
      return revng::createError("cannot find C support header: " + Name);
    return errorOrToExpected(llvm::MemoryBuffer::getFile(*Path));
  };
  auto Attributes = ReadResource("attributes.h");
  if (not Attributes) {
    llvmErrorToRpError(Attributes.takeError(), error);
    return nullptr;
  }
  auto PrimitiveTypes = ReadResource("primitive-types.h");
  if (not PrimitiveTypes) {
    llvmErrorToRpError(PrimitiveTypes.takeError(), error);
    return nullptr;
  }

  auto Result = std::make_unique<rp_buffer>();
  llvm::raw_svector_ostream Stream(*Result);
  revng::GzipTarWriter Writer(Stream);
  auto AppendBuffer = [&](llvm::StringRef Path, llvm::ArrayRef<char> Data) {
    Writer.append(Path, Data);
  };
  AppendBuffer("decompiled/functions.c", *Functions);
  AppendBuffer("decompiled/types-and-globals.h", *Types);
  AppendBuffer("decompiled/helpers.h", *Helpers);
  llvm::StringRef AttributesData = (*Attributes)->getBuffer();
  AppendBuffer("decompiled/attributes.h",
               {AttributesData.data(), AttributesData.size()});
  llvm::StringRef PrimitiveTypesData = (*PrimitiveTypes)->getBuffer();
  AppendBuffer("decompiled/primitive-types.h",
               {PrimitiveTypesData.data(), PrimitiveTypesData.size()});
  Writer.close();
  return Result.release();
}

static rp_buffer *_rp_manager_decompile_function_to_ptml(rp_manager *manager,
                                                         const char *address,
                                                         rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(address != nullptr);
  return decompilePTML(manager, address, error);
}

static rp_buffer *_rp_manager_decompile_function_to_c(rp_manager *manager,
                                                      const char *address,
                                                      rp_error *error) {
  revng_check(manager != nullptr);
  revng_check(address != nullptr);
  std::unique_ptr<rp_buffer> PTML(decompilePTML(manager, address, error));
  return PTML ? stripPTML(*PTML).release() : nullptr;
}

// NOLINTEND

// Import the autogenerated wrappers, these will contains calls to the
// `wrap<>()` function defined in `Wrapper.h` that will forward the arguments to
// the function of the same name in this file, with an underscore in front.
// For example: rp_initialize will call
// `wrap<"rp_intialize">(rp_initialize, ...)`
// which in turn will call `_rp_initialize`
#include "revng/PipelineC/Wrappers.h"
