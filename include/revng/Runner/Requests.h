#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <set>
#include <vector>

#include "revng/PipeboxCommon/Common.h"
#include "revng/PipeboxCommon/Model.h"
#include "revng/PipeboxCommon/ObjectID.h"

namespace revng::runner {

/// A set of objects, all of one kind.
///
/// The kind is carried alongside because an empty set still has to say what it
/// is a set *of*: a request for no functions and a request for no type
/// definitions travel differently through the pipeline.
class ObjectSet {
private:
  Kind TheKind = Kinds::Binary;
  std::set<ObjectID> Objects;

public:
  ObjectSet() = default;
  explicit ObjectSet(Kind TheKind) : TheKind(TheKind) {}
  ObjectSet(Kind TheKind, std::set<ObjectID> Objects) :
    TheKind(TheKind), Objects(std::move(Objects)) {}

public:
  Kind kind() const { return TheKind; }
  bool empty() const { return Objects.empty(); }
  size_t size() const { return Objects.size(); }
  const std::set<ObjectID> &objects() const { return Objects; }

  auto begin() const { return Objects.begin(); }
  auto end() const { return Objects.end(); }

  void insert(const ObjectID &Object) {
    revng_assert(Object.kind() == TheKind);
    Objects.insert(Object);
  }

  void merge(const ObjectSet &Other) {
    revng_assert(Other.TheKind == TheKind);
    Objects.insert(Other.Objects.begin(), Other.Objects.end());
  }

  bool contains(const ObjectID &Object) const {
    return Objects.count(Object) != 0;
  }

  /// The objects of this set that `Other` does not already hold.
  ObjectSet subtract(const std::set<ObjectID> &Other) const;
};

/// Translate a request between granularities.
///
/// Kinds form a two-level tree: `Binary` is the root, `Function` and
/// `TypeDefinition` are its children and are unrelated to each other. So asking
/// for a function from a container that holds the whole binary means asking for
/// the binary, and the other way round means asking the model which functions
/// it has.
ObjectSet moveToKind(const Model &TheModel,
                     const ObjectSet &Objects,
                     Kind Destination);

/// What is wanted out of each container, keyed by its index in
/// `PipelineDescription::Declarations`.
class Requests {
private:
  std::map<size_t, ObjectSet> Map;

public:
  bool empty() const;
  bool contains(size_t Declaration) const { return Map.count(Declaration) != 0; }
  auto begin() const { return Map.begin(); }
  auto end() const { return Map.end(); }

  const ObjectSet *find(size_t Declaration) const {
    auto It = Map.find(Declaration);
    return It == Map.end() ? nullptr : &It->second;
  }

  void set(size_t Declaration, ObjectSet Objects) {
    Map.insert_or_assign(Declaration, std::move(Objects));
  }

  void merge(size_t Declaration, const ObjectSet &Objects);
  void merge(const Requests &Other);

  /// Drop the entries that ask for nothing, so an empty result is recognisable
  /// as "there is nothing left to do".
  Requests minimize() const;
};

} // namespace revng::runner
