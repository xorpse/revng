#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace revng::runner {

/// The bytes behind the binaries a model refers to.
///
/// Upstream resolves `model::BinaryIdentifier`s through a `FileProvider` on the
/// Python side. An embedder has the bytes in its own address space instead, so
/// it deposits them here and the native `import-files` pipe reads them back.
/// Keyed by the identifier's hash, which is how the model names them.
class FileStore {
public:
  static FileStore &get();

public:
  /// Store `Data` under `Hash`, replacing anything already there.
  void add(llvm::StringRef Hash, llvm::ArrayRef<char> Data);
  /// The bytes for `Hash`, or nothing if the embedder never provided them.
  std::optional<llvm::ArrayRef<char>> find(llvm::StringRef Hash) const;
  void erase(llvm::StringRef Hash);

private:
  mutable std::mutex Mutex;
  llvm::StringMap<std::vector<char>> Files;
};

} // namespace revng::runner
