//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Runner/Requests.h"

namespace revng::runner {

ObjectSet ObjectSet::subtract(const std::set<ObjectID> &Other) const {
  ObjectSet Result(TheKind);
  for (const ObjectID &Object : Objects)
    if (Other.count(Object) == 0)
      Result.insert(Object);
  return Result;
}

ObjectSet moveToKind(const Model &TheModel,
                     const ObjectSet &Objects,
                     Kind Destination) {
  if (Objects.empty())
    return ObjectSet(Destination);

  Kind Source = Objects.kind();
  if (Source == Destination)
    return Objects;

  // Everything hangs off the binary, so moving up is just "the binary", and
  // moving down is "ask the model what it has".
  if (Destination == Kinds::Binary)
    return ObjectSet(Destination, { ObjectID::root() });

  if (Source == Kinds::Binary) {
    ObjectSet Result(Destination);
    for (const ObjectID &Object : Objects)
      for (const ObjectID &Child : TheModel.children(Object, Destination))
        Result.insert(Child);
    return Result;
  }

  // `Function` and `TypeDefinition` are siblings: one says nothing about the
  // other, so a request cannot be carried across.
  return ObjectSet(Destination);
}

bool Requests::empty() const {
  for (const auto &[Declaration, Objects] : Map)
    if (not Objects.empty())
      return false;
  return true;
}

void Requests::merge(size_t Declaration, const ObjectSet &Objects) {
  auto It = Map.find(Declaration);
  if (It == Map.end())
    Map.emplace(Declaration, Objects);
  else
    It->second.merge(Objects);
}

void Requests::merge(const Requests &Other) {
  for (const auto &[Declaration, Objects] : Other.Map)
    merge(Declaration, Objects);
}

Requests Requests::minimize() const {
  Requests Result;
  for (const auto &[Declaration, Objects] : Map)
    if (not Objects.empty())
      Result.set(Declaration, Objects);
  return Result;
}

} // namespace revng::runner
