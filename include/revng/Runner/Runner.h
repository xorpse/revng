#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <memory>
#include <vector>

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"

#include "revng/PipeboxCommon/Helpers/Native/Container.h"
#include "revng/PipeboxCommon/Model.h"
#include "revng/Runner/Initialize.h"
#include "revng/Runner/MemoryStorage.h"
#include "revng/Runner/PipelineDescription.h"
#include "revng/Runner/Requests.h"

namespace revng::runner {

/// Runs the pipeline in this process.
///
/// Upstream drives the pipeline from Python, dispatching each task to a
/// short-lived worker that exchanges containers as files on disk. A host that
/// serves the input through its own callbacks cannot use that, because those
/// callbacks do not survive a `fork`/`exec`. This walks the same description
/// in-process instead.
///
/// Containers are deliberately not kept alive between runs. The same container
/// declaration appears at several savepoints holding different content --
/// `llvm-functions` is pre-ABI at `isolate` and post-ABI at `enforce-abi` --
/// so a long-lived instance would hand a later pipe the wrong stage's contents
/// depending on the order calls happened to arrive in, and produce wrong output
/// rather than an error. State lives in `MemoryStorage`, keyed by savepoint.
class Runner {
public:
  Runner(const PipelineDescription &Description, Model &TheModel) :
    Description(Description), TheModel(TheModel) {
    // Pipes look LLVM passes up by name in a registry that is empty until
    // something fills it, and an embedder has no other reason to know that.
    ensureLLVMInitialized();
  }

public:
  /// Run whatever is needed so that `Wanted` is available at `Target`.
  llvm::Error produce(const PipelineNode *Target, const Requests &Wanted);

  /// Produce a single object and return its serialised bytes.
  llvm::Expected<revng::pypeline::Buffer>
  produceOne(const PipelineNode *Target, size_t Declaration,
             const ObjectID &Object);

  /// Produce a named artifact, for every object it can offer.
  llvm::Expected<revng::pypeline::Buffer>
  produceArtifact(llvm::StringRef ArtifactName, const ObjectID &Object);

  /// Run a named analysis, which may mutate the model.
  llvm::Error runAnalysis(llvm::StringRef Name,
                          llvm::StringRef ConfigurationYAML);
  llvm::Error runAnalysisList(llvm::StringRef Name,
                              llvm::StringRef ConfigurationYAML);

  /// Per-pipe configuration handed to the pipe at each run, on top of the
  /// static configuration the description carries.
  void setDynamicConfiguration(llvm::StringRef PipeName,
                               llvm::StringRef Configuration) {
    DynamicConfiguration[PipeName] = Configuration.str();
  }

  /// Forget everything cached from `Savepoint` onwards. Used after the model
  /// changes, or after an embedder edits a container in place.
  void invalidateFrom(size_t Savepoint) { Storage.eraseFrom(Savepoint); }
  void invalidate(const std::set<std::string> &ChangedPaths) {
    Storage.invalidate(ChangedPaths);
  }

  /// How many pipes have actually been executed. A request served entirely
  /// from the savepoint cache leaves this unchanged.
  size_t pipesRun() const { return PipesRun; }

  MemoryStorage &storage() { return Storage; }
  const PipelineDescription &description() const { return Description; }

  /// The kind of the objects a declared container holds.
  llvm::Expected<Kind> kindOf(size_t Declaration) const;

private:
  /// One task to run, with what it should read and what it should produce.
  struct ScheduledTask {
    const PipelineNode *Node = nullptr;
    Requests Incoming;
    Requests Outgoing;
  };

  /// Walk back from `Target` working out what each task has to read, stopping
  /// where storage already holds the answer.
  llvm::Expected<std::vector<ScheduledTask>>
  schedule(const PipelineNode *Target, const Requests &Wanted);

  llvm::Error runSchedule(llvm::ArrayRef<ScheduledTask> Tasks);

  /// What a task must read in order to produce `Wanted`.
  llvm::Expected<Requests> prerequisitesFor(const PipelineNode *Node,
                                            const Requests &Wanted);

  /// The containers this node is responsible for filling, so that a request
  /// for anything else keeps travelling back to whatever does fill it.
  llvm::Expected<std::set<size_t>> producedBy(const PipelineNode *Node);

private:
  const PipelineDescription &Description;
  Model &TheModel;
  MemoryStorage Storage;
  llvm::StringMap<std::string> DynamicConfiguration;
  /// Cached so that asking for a container's kind does not construct one:
  /// `LLVMRootContainer`'s constructor allocates an `LLVMContext`.
  mutable llvm::StringMap<Kind> KindCache;
  size_t PipesRun = 0;
};

} // namespace revng::runner
