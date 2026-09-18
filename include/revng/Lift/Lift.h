#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>
#include <string>

#include "revng/PipeboxCommon/BinariesContainer.h"
#include "revng/PipeboxCommon/LLVMContainer.h"
#include "revng/PipeboxCommon/Model.h"

namespace revng::pypeline::piperuns {

class Lift {
public:
  static constexpr llvm::StringRef Name = "lift";
  using Arguments = TypeList<
    PipeRunArgument<const BinariesContainer, "Input", "Input binaries to lift">,
    PipeRunArgument<LLVMRootContainer,
                    "Output",
                    "LLVM Module containing the lifted binaries",
                    Access::Write>>;

public:
  /// Lifting options carried by the pipe's own configuration.
  ///
  /// A pipe instance is configured per invocation, so this cannot be a
  /// process-global `llvm::cl::opt`: an embedder holds one process open across
  /// many lifts, and `pypeline-run-pipe` builds its arguments from the pipeline
  /// description rather than from a command line.
  struct Configuration {
    /// Registered backend to lift with; empty selects the registry default.
    std::string Backend;
    /// Overrides the entry point recorded in the model.
    std::optional<uint64_t> EntryPoint;

    static Configuration parse(llvm::StringRef Serialized);
  };

private:
  const Model &TheModel;
  const BinariesContainer &Binary;
  LLVMRootContainer &ModuleContainer;
  Configuration Options;

public:
  Lift(const class Model &Model,
       llvm::StringRef Config,
       llvm::StringRef DynamicConfig,
       const BinariesContainer &Binary,
       LLVMRootContainer &ModuleContainer);

  CustomInvalidationData run();

public:
  static llvm::Error checkPrecondition(const class Model &Model);

  static bool requiresCustomInvalidation(const ModelDiff &Diff);

  static std::vector<std::set<ObjectID>>
  processCustomInvalidation(const InvalidationData &Data,
                            const ModelDiff &Diff);
};

} // namespace revng::pypeline::piperuns
