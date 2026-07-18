#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>
#include <optional>
#include <string>

#include "llvm/ADT/ArrayRef.h"

#include "revng/Model/Architecture.h"
#include "revng/Support/MetaAddress.h"

namespace revng::loader {

/// A mapped region in an already-loaded target address space.
///
/// Contents can be shorter than VirtualSize. The remainder is represented as
/// unbacked memory (for example, a .bss region) in the generated model.
struct Mapping {
  MetaAddress Start;
  uint64_t VirtualSize = 0;
  llvm::ArrayRef<uint8_t> Contents;
  bool Readable = true;
  bool Writeable = false;
  bool Executable = false;
  std::string Name;
  /// Number of bytes backed by the provider. For ordinary mappings this is
  /// inferred from Contents. A non-zero value permits metadata-only mappings.
  uint64_t BackingSize = 0;
};

/// Host-provided description of a program that has already been loaded.
class AbstractAddressSpace {
public:
  virtual ~AbstractAddressSpace() = default;

  virtual model::Architecture::Values architecture() const = 0;
  virtual std::optional<MetaAddress> entryPoint() const { return std::nullopt; }
  virtual llvm::ArrayRef<Mapping> mappings() const = 0;
  virtual llvm::ArrayRef<MetaAddress> extraCodeAddresses() const { return {}; }
};

} // namespace revng::loader
