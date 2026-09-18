//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <mutex>
#include <string>

#include "llvm/ADT/STLExtras.h"

#include "revng/Lift/AbstractLifter.h"
#include "revng/Support/Error.h"

namespace revng::lift {
namespace {

struct RegistryStorage {
  std::mutex Mutex;
  std::map<std::string, LifterFactory, std::less<>> Factories;
  std::map<std::string,
           std::vector<model::Architecture::Values>,
           std::less<>> SupportedArchitectures;
  std::map<const model::Binary *, LifterFactory> Overrides;
  std::string Default;
};

RegistryStorage &storage() {
  static RegistryStorage Storage;
  return Storage;
}

} // namespace

llvm::Error LifterRegistry::registerLifter(llvm::StringRef Name,
                                           LifterFactory Factory,
                                           bool MakeDefault,
                                           llvm::ArrayRef<model::Architecture::Values>
                                             SupportedArchitectures) {
  if (Name.empty())
    return revng::createError("a lifter backend must have a name");
  if (not Factory)
    return revng::createError("a lifter backend must have a factory");

  RegistryStorage &Storage = storage();
  std::lock_guard Lock(Storage.Mutex);
  auto [Iterator, Inserted] = Storage.Factories.emplace(Name.str(),
                                                        std::move(Factory));
  if (not Inserted)
    return revng::createError("lifter backend is already registered: " + Name);
  Storage.SupportedArchitectures[Iterator->first] =
    std::vector(SupportedArchitectures.begin(), SupportedArchitectures.end());
  if (MakeDefault)
    Storage.Default = Iterator->first;
  return llvm::Error::success();
}

llvm::Expected<std::unique_ptr<ILifter>>
LifterRegistry::create(llvm::StringRef Name,
                       const TupleTree<model::Binary> &Model) {
  LifterFactory Factory;
  {
    RegistryStorage &Storage = storage();
    std::lock_guard Lock(Storage.Mutex);
    auto Iterator = Storage.Factories.find(Name.str());
    if (Iterator == Storage.Factories.end())
      return revng::createError("unknown lifter backend: " + Name);
    Factory = Iterator->second;
  }

  std::unique_ptr<ILifter> Result = Factory(Model);
  if (not Result)
    return revng::createError("lifter backend factory returned null: " + Name);
  return Result;
}

llvm::Expected<std::unique_ptr<ILifter>>
LifterRegistry::createDefault(const TupleTree<model::Binary> &Model) {
  LifterFactory Override;
  std::string Name;
  {
    RegistryStorage &Storage = storage();
    std::lock_guard Lock(Storage.Mutex);
    auto OverrideIterator = Storage.Overrides.find(Model.get());
    if (OverrideIterator != Storage.Overrides.end())
      Override = OverrideIterator->second;
    Name = Storage.Default;
  }
  if (Override) {
    std::unique_ptr<ILifter> Result = Override(Model);
    if (not Result)
      return revng::createError("model-specific lifter factory returned null");
    return Result;
  }
  if (Name.empty())
    return revng::createError("no lifter backend is registered");
  return create(Name, Model);
}

llvm::Error LifterRegistry::setOverride(const model::Binary &Binary,
                                        LifterFactory Factory) {
  if (not Factory)
    return revng::createError("a model-specific lifter must have a factory");
  RegistryStorage &Storage = storage();
  std::lock_guard Lock(Storage.Mutex);
  Storage.Overrides[&Binary] = std::move(Factory);
  return llvm::Error::success();
}

void LifterRegistry::clearOverride(const model::Binary &Binary) {
  RegistryStorage &Storage = storage();
  std::lock_guard Lock(Storage.Mutex);
  Storage.Overrides.erase(&Binary);
}

llvm::StringRef LifterRegistry::defaultLifter() {
  static thread_local std::string Snapshot;
  RegistryStorage &Storage = storage();
  std::lock_guard Lock(Storage.Mutex);
  Snapshot = Storage.Default;
  return Snapshot;
}

bool LifterRegistry::empty() {
  RegistryStorage &Storage = storage();
  std::lock_guard Lock(Storage.Mutex);
  return Storage.Factories.empty();
}

bool LifterRegistry::hasLifter(const model::Binary &Binary) {
  RegistryStorage &Storage = storage();
  std::lock_guard Lock(Storage.Mutex);
  return not Storage.Factories.empty() or Storage.Overrides.contains(&Binary);
}

std::vector<std::string> LifterRegistry::names() {
  RegistryStorage &Storage = storage();
  std::lock_guard Lock(Storage.Mutex);
  std::vector<std::string> Result;
  Result.reserve(Storage.Factories.size());
  for (const auto &[Name, Factory] : Storage.Factories)
    Result.push_back(Name);
  return Result;
}

bool LifterRegistry::supportsArchitecture(
  llvm::StringRef Name,
  model::Architecture::Values Architecture) {
  RegistryStorage &Storage = storage();
  std::lock_guard Lock(Storage.Mutex);
  auto Iterator = Storage.SupportedArchitectures.find(Name.str());
  if (Iterator == Storage.SupportedArchitectures.end())
    return false;
  const auto &Supported = Iterator->second;
  return Supported.empty() or llvm::is_contained(Supported, Architecture);
}

} // namespace revng::lift
