#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"

#include "revng/Lift/AbstractLifter.h"
#include "revng/Model/RawBinaryProvider.h"
#include "revng/PipeboxCommon/Model.h"
#include "revng/Runner/PipelineDescription.h"
#include "revng/Runner/Runner.h"

namespace revng::embed {

/// A container as the C API addresses it: which one, and at which point of the
/// pipeline. The pair is needed because the same declaration holds different
/// content at different savepoints.
struct ContainerHandle {
  const runner::PipelineNode *Node = nullptr;
  size_t Declaration = 0;

  bool operator==(const ContainerHandle &) const = default;
  std::strong_ordering operator<=>(const ContainerHandle &) const = default;
};

/// What the C API calls a target: an object, and the granularity it is named
/// at. `Kind` used to be an open hierarchy; it is now one of three values, and
/// the object carries its own kind, so this is mostly a compatibility shim.
struct TargetHandle {
  ObjectID Object;

  Kind kind() const { return Object.kind(); }
};

using ContainerTargetsMap = std::map<ContainerHandle, std::vector<TargetHandle>>;

/// Build an object from a kind and the bare key the C API names it by, for
/// example `0x1000:Code_x86_64`. A full location path is accepted too.
std::optional<ObjectID> objectFromKey(Kind TheKind, llvm::StringRef Key);

/// Intern a container handle in a process-global table.
///
/// `rp_step_get_container` has no manager to hang the handle off, and the C API
/// returns a bare pointer that the caller may keep, so the handles live as long
/// as the process. They are two words each and bounded by the pipeline's size.
const ContainerHandle *internContainerHandle(ContainerHandle Handle);

/// Everything one embedding session owns.
///
/// Replaces the old `revng::pipes::PipelineManager`. It holds the model, the
/// parsed pipeline and the runner, and keeps the side tables that are keyed on
/// the live model pointer in step with it.
class Manager {
public:
  static llvm::Expected<std::unique_ptr<Manager>>
  create(llvm::StringRef PipelinePath);
  static llvm::Expected<std::unique_ptr<Manager>>
  createFromString(llvm::StringRef PipelineYAML);

  ~Manager();

  Manager(const Manager &) = delete;
  Manager &operator=(const Manager &) = delete;

public:
  Model &model() { return TheModel; }
  const Model &model() const { return TheModel; }
  runner::Runner &runner() { return *TheRunner; }
  const runner::PipelineDescription &description() const {
    return Description;
  }

  /// Call after mutating the model.
  ///
  /// Anything computed from the model is now stale, and the side tables keyed
  /// on the model's root pointer -- the raw binary provider and the lifter
  /// override -- have to follow it if `TupleTree`'s root moved, which happens
  /// when the whole tree is replaced rather than edited in place.
  void onModelChanged();

  uint64_t commitIndex() const { return CommitIndex; }

  /// Keep a byte source alive for as long as this manager, and registered
  /// against the current model.
  void adoptBinaryProvider(std::shared_ptr<RawBinaryProvider> Provider);

  /// Drop the byte source, so a host that handed one over learns it is no
  /// longer needed. Called once the bytes have been materialised.
  void releaseBinaryProvider();

  /// Route lifting through a host-supplied backend for as long as this manager
  /// lives, re-registering it if the model root moves.
  llvm::Error adoptLifterFactory(revng::lift::LifterFactory Factory);

  /// Intern a container handle so the C API can hand out a stable pointer.
  const ContainerHandle *internContainer(ContainerHandle Handle);

  /// The manager that owns this pipeline node.
  ///
  /// Several C entry points -- `rp_target_is_ready`, `rp_container_get_mime`,
  /// `rp_container_extract_one` -- take only a container handle, but need the
  /// manager's state. Rather than change those signatures, every manager
  /// registers its nodes here so a handle can find its way back.
  static Manager *ownerOf(const runner::PipelineNode *Node);

  /// The MIME type of a declared container, cached because asking the registry
  /// means constructing one, and `LLVMRootContainer` allocates a context.
  llvm::StringRef mimeTypeOf(size_t Declaration);

  /// The pipeline description, as the C API hands it out.
  const std::string &serializedDescription();

private:
  Manager() = default;
  void registerSideTables();
  void unregisterSideTables();
  void registerNodes();
  void unregisterNodes();

private:
  runner::PipelineDescription Description;
  Model TheModel;
  std::optional<runner::Runner> TheRunner;
  std::shared_ptr<RawBinaryProvider> Provider;
  /// A host lifter, held so it can follow the model root. The registry keys
  /// overrides on the root address, and discarding one silently reverts
  /// lifting to the built-in backend.
  std::optional<revng::lift::LifterFactory> LifterOverride;
  /// The model as the pipeline last saw it, to diff an in-place edit against.
  std::optional<Model> PreImage;
  /// The model root the side tables were last registered against.
  const model::Binary *RegisteredRoot = nullptr;
  uint64_t CommitIndex = 0;
  std::vector<std::unique_ptr<ContainerHandle>> InternedContainers;
  llvm::StringMap<std::string> MimeTypes;
  std::optional<std::string> SerializedDescription;
};

} // namespace revng::embed
