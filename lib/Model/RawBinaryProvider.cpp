//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <mutex>

#include "revng/Model/RawBinaryProvider.h"

namespace {

struct ProviderRegistry {
  std::mutex Mutex;
  std::map<const model::Binary *, std::shared_ptr<RawBinaryProvider>> Providers;
};

ProviderRegistry &registry() {
  static ProviderRegistry Registry;
  return Registry;
}

} // namespace

void registerRawBinaryProvider(const model::Binary &Binary,
                               std::shared_ptr<RawBinaryProvider> Provider) {
  ProviderRegistry &Registry = registry();
  std::lock_guard Lock(Registry.Mutex);
  Registry.Providers[&Binary] = std::move(Provider);
}

void unregisterRawBinaryProvider(const model::Binary &Binary) {
  ProviderRegistry &Registry = registry();
  std::lock_guard Lock(Registry.Mutex);
  Registry.Providers.erase(&Binary);
}

std::shared_ptr<RawBinaryProvider>
findRawBinaryProvider(const model::Binary &Binary) {
  ProviderRegistry &Registry = registry();
  std::lock_guard Lock(Registry.Mutex);
  auto Iterator = Registry.Providers.find(&Binary);
  return Iterator == Registry.Providers.end() ? nullptr : Iterator->second;
}
