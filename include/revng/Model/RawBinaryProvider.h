#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>
#include <memory>
#include <optional>

#include "llvm/ADT/ArrayRef.h"

namespace model {
class Binary;
}

/// A stable, potentially lazy source for the flattened bytes described by a
/// model::Binary. Returned ranges must remain valid for the provider lifetime.
class RawBinaryProvider {
public:
  virtual ~RawBinaryProvider() = default;

  virtual uint64_t size() const = 0;
  virtual std::optional<llvm::ArrayRef<uint8_t>>
  getByOffset(uint64_t Offset, uint64_t Size) const = 0;

  std::optional<llvm::ArrayRef<uint8_t>> bytes() const {
    return getByOffset(0, size());
  }
};

/// Associate a provider with a live model. RawBinaryView instances created for
/// that model discover it automatically. Registration does not modify the
/// serializable model and must be removed before the model is destroyed.
void registerRawBinaryProvider(const model::Binary &Binary,
                               std::shared_ptr<RawBinaryProvider> Provider);
void unregisterRawBinaryProvider(const model::Binary &Binary);
std::shared_ptr<RawBinaryProvider>
findRawBinaryProvider(const model::Binary &Binary);
