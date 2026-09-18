#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <string>
#include <set>
#include <vector>

#include "revng/PipeboxCommon/Common.h"
#include "revng/PipeboxCommon/ObjectID.h"
#include "revng/Runner/Requests.h"

namespace revng::runner {

/// Where a stored object came from.
///
/// The container declaration alone is not enough: `llvm-functions` appears at
/// several savepoints holding different content each time -- pre-ABI at
/// `isolate`, post-ABI at `enforce-abi`, and so on. Conflating them would hand
/// a later pipe the wrong stage's IR, which produces wrong output rather than
/// an error.
struct ContainerLocation {
  size_t Savepoint = 0;
  size_t Declaration = 0;

  std::strong_ordering operator<=>(const ContainerLocation &) const = default;
  bool operator==(const ContainerLocation &) const = default;
};

/// Serialised objects, cached at the savepoints that declared them.
/// An object, and the savepoints it may have been stored at.
struct DependencyEntry {
  size_t SavepointStart = 0;
  size_t SavepointEnd = 0;
  size_t Declaration = 0;
  ObjectID Object;

  std::strong_ordering operator<=>(const DependencyEntry &) const = default;
  bool operator==(const DependencyEntry &) const = default;
};

class MemoryStorage {
public:
  /// Which of `Wanted` is already held here.
  std::set<ObjectID> has(const ContainerLocation &Location,
                         const ObjectSet &Wanted) const;

  /// The subset of `Wanted` that still has to be produced.
  ObjectSet missing(const ContainerLocation &Location,
                    const ObjectSet &Wanted) const;

  void add(const ContainerLocation &Location,
           std::map<ObjectID, revng::pypeline::Buffer> Objects);

  /// The stored bytes for `Wanted`, in the shape `Container::deserialize` takes.
  /// The returned references borrow from this storage, which must outlive them.
  std::map<const ObjectID *, llvm::ArrayRef<char>>
  get(const ContainerLocation &Location, const ObjectSet &Wanted) const;

  std::set<ObjectID> objectsAt(const ContainerLocation &Location) const;

  void erase(const ContainerLocation &Location,
             const std::set<ObjectID> &Objects);
  /// Drop everything cached at savepoints after `Savepoint`, which is what an
  /// edit to an earlier stage invalidates.
  void eraseFrom(size_t Savepoint);
  void clear() {
    Objects.clear();
    Dependencies.clear();
  }

  void dependOn(llvm::StringRef Path, DependencyEntry Entry);

  /// Drop the objects whose production read one of \p ChangedPaths.
  void invalidate(const std::set<std::string> &ChangedPaths);

private:
  std::map<ContainerLocation, std::map<ObjectID, revng::pypeline::Buffer>>
    Objects;
  std::map<std::string, std::set<DependencyEntry>> Dependencies;
};

} // namespace revng::runner
