//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Runner/MemoryStorage.h"

namespace revng::runner {

std::set<ObjectID> MemoryStorage::has(const ContainerLocation &Location,
                                      const ObjectSet &Wanted) const {
  std::set<ObjectID> Result;
  auto It = Objects.find(Location);
  if (It == Objects.end())
    return Result;

  for (const ObjectID &Object : Wanted)
    if (It->second.count(Object) != 0)
      Result.insert(Object);

  return Result;
}

ObjectSet MemoryStorage::missing(const ContainerLocation &Location,
                                 const ObjectSet &Wanted) const {
  return Wanted.subtract(has(Location, Wanted));
}

void MemoryStorage::add(const ContainerLocation &Location,
                        std::map<ObjectID, revng::pypeline::Buffer> NewObjects) {
  auto &Slot = Objects[Location];
  for (auto &[Object, Data] : NewObjects)
    Slot.insert_or_assign(Object, std::move(Data));
}

std::map<const ObjectID *, llvm::ArrayRef<char>>
MemoryStorage::get(const ContainerLocation &Location,
                   const ObjectSet &Wanted) const {
  std::map<const ObjectID *, llvm::ArrayRef<char>> Result;
  auto It = Objects.find(Location);
  if (It == Objects.end())
    return Result;

  for (const auto &[Object, Data] : It->second)
    if (Wanted.contains(Object))
      Result.emplace(&Object, Data.data());

  return Result;
}

std::set<ObjectID>
MemoryStorage::objectsAt(const ContainerLocation &Location) const {
  std::set<ObjectID> Result;
  auto It = Objects.find(Location);
  if (It == Objects.end())
    return Result;

  for (const auto &[Object, Data] : It->second)
    Result.insert(Object);

  return Result;
}

void MemoryStorage::erase(const ContainerLocation &Location,
                          const std::set<ObjectID> &ToErase) {
  auto It = Objects.find(Location);
  if (It == Objects.end())
    return;

  for (const ObjectID &Object : ToErase)
    It->second.erase(Object);
}

void MemoryStorage::eraseFrom(size_t Savepoint) {
  for (auto It = Objects.begin(); It != Objects.end();) {
    if (It->first.Savepoint > Savepoint)
      It = Objects.erase(It);
    else
      ++It;
  }
}

void MemoryStorage::dependOn(llvm::StringRef Path, DependencyEntry Entry) {
  Dependencies[Path.str()].insert(std::move(Entry));
}

void MemoryStorage::invalidate(const std::set<std::string> &ChangedPaths) {
  std::set<DependencyEntry> Stale;
  for (const std::string &Path : ChangedPaths) {
    auto It = Dependencies.find(Path);
    if (It == Dependencies.end())
      continue;

    Stale.insert(It->second.begin(), It->second.end());
    Dependencies.erase(It);
  }

  for (const DependencyEntry &Entry : Stale) {
    for (auto &[Location, Stored] : Objects) {
      if (Location.Declaration != Entry.Declaration)
        continue;
      if (Location.Savepoint < Entry.SavepointStart
          or Location.Savepoint > Entry.SavepointEnd)
        continue;

      Stored.erase(Entry.Object);
    }
  }
}

} // namespace revng::runner
