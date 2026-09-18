#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include "revng/Model/Binary.h"
#include "revng/Model/RawBinaryView.h"
#include "revng/TupleTree/TupleTree.h"

namespace llvm {
class Module;
}

namespace revng::lift {

/// A machine-code to LLVM IR backend.
///
/// Implementations must produce the module shape documented in
/// docs/lifter-contract.md. They may use Entries as lifting seeds, but can lift
/// a larger reachable program when that is more natural for the backend.
class ILifter {
public:
  virtual ~ILifter() = default;

  virtual llvm::Error lift(const model::Binary &Binary,
                           const RawBinaryView &View,
                           llvm::ArrayRef<MetaAddress> Entries,
                           llvm::Module &Output) = 0;
};

using LifterFactory = std::function<
  std::unique_ptr<ILifter>(const TupleTree<model::Binary> &)>;

class LifterRegistry {
public:
  static llvm::Error registerLifter(llvm::StringRef Name,
                                    LifterFactory Factory,
                                    bool MakeDefault = false,
                                    llvm::ArrayRef<model::Architecture::Values>
                                      SupportedArchitectures = {});
  static llvm::Expected<std::unique_ptr<ILifter>>
  create(llvm::StringRef Name, const TupleTree<model::Binary> &Model);
  static llvm::Expected<std::unique_ptr<ILifter>>
  createDefault(const TupleTree<model::Binary> &Model);
  /// Set a backend for one model without affecting other pipeline managers.
  static llvm::Error setOverride(const model::Binary &Binary,
                                 LifterFactory Factory);
  static void clearOverride(const model::Binary &Binary);
  static llvm::StringRef defaultLifter();
  static bool empty();
  static bool hasLifter(const model::Binary &Binary);
  static std::vector<std::string> names();
  static bool supportsArchitecture(llvm::StringRef Name,
                                   model::Architecture::Values Architecture);
};

} // namespace revng::lift
