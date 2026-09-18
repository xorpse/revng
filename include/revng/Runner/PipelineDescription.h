#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include "revng/PipeboxCommon/Common.h"
#include "revng/PipeboxCommon/ObjectID.h"

namespace revng::runner {

/// A container the pipeline declares, and the registered type backing it.
///
/// The same type can back several declarations: `llvm-functions` and
/// `llvm-root-with-functions` are both `LLVMFunctionContainer`s holding
/// different content at different points of the pipeline.
struct ContainerDeclaration {
  std::string Name;
  std::string TypeName;
  /// Its own position in `PipelineDescription::Declarations`, so a handle to
  /// one can be turned back into the index everything else is keyed by.
  size_t Index = 0;
};

/// One container argument of a task, as an index into
/// `PipelineDescription::Declarations` plus how the task uses it.
struct TaskArgument {
  size_t Declaration = 0;
  revng::pypeline::Access Access = revng::pypeline::Access::Auto;
};

/// Runs a registered pipe over the task's arguments.
struct PipeTask {
  std::string PipeName;
  /// Serialised YAML, handed to the pipe's constructor verbatim. The schema
  /// allows either a string or a mapping; a mapping is re-serialised.
  std::string StaticConfiguration;
};

/// Materialises the named containers so later tasks can start from here
/// without re-running everything before it.
struct SavepointTask {
  std::string Name;
  /// Indices into `Declarations`.
  std::vector<size_t> ToSave;
};

/// A node of the pipeline tree.
///
/// Branches form a tree rather than a general DAG: a branch either starts from
/// the root or continues exactly one other branch, so every node has at most
/// one predecessor. That is what lets the scheduler walk backwards from a
/// target without a general graph search.
/// Savepoints are numbered depth-first, so a node's subtree is an interval and
/// "everything downstream" is a range test.
struct SavepointRange {
  size_t Start = 0;
  size_t End = 0;
};

struct PipelineNode {
  /// Depth-first index, set on savepoints; storage is keyed by it.
  size_t Id = 0;
  SavepointRange Downstream;
  std::variant<PipeTask, SavepointTask> Task;
  std::vector<TaskArgument> Arguments;
  PipelineNode *Predecessor = nullptr;
  std::vector<PipelineNode *> Successors;

  bool isSavepoint() const {
    return std::holds_alternative<SavepointTask>(Task);
  }
  const PipeTask &pipe() const { return std::get<PipeTask>(Task); }
  const SavepointTask &savepoint() const {
    return std::get<SavepointTask>(Task);
  }
  llvm::StringRef name() const {
    return isSavepoint() ? llvm::StringRef(savepoint().Name) :
                           llvm::StringRef(pipe().PipeName);
  }
};

/// A named output: the container to read, at the node that produced it.
struct Artifact {
  std::string Name;
  std::string Description;
  std::string Category;
  std::string Filename;
  PipelineNode *Node = nullptr;
  size_t Declaration = 0;
};

/// An analysis, bound to the node whose containers it reads.
struct AnalysisBinding {
  std::string Name;
  std::string Description;
  /// Null for the analyses declared at the top level, which read no container
  /// and therefore run against the pipeline root.
  PipelineNode *Node = nullptr;
  std::vector<size_t> Arguments;
};

class PipelineDescription {
public:
  std::vector<ContainerDeclaration> Declarations;
  /// Owns every node; `Root` and the pointers in the nodes point into this.
  std::vector<std::unique_ptr<PipelineNode>> Nodes;
  llvm::StringMap<Artifact> Artifacts;
  llvm::StringMap<AnalysisBinding> Analyses;
  llvm::StringMap<std::vector<std::string>> AnalysisLists;

public:
  static llvm::Expected<PipelineDescription> parse(llvm::StringRef YAML);
  static llvm::Expected<PipelineDescription> fromFile(llvm::StringRef Path);

  void assignSavepointRanges();

public:
  /// Index of the declaration with this name, if any.
  std::optional<size_t> findDeclaration(llvm::StringRef Name) const;
  /// The savepoint with this name, if any.
  const PipelineNode *findSavepoint(llvm::StringRef Name) const;
  const Artifact *findArtifact(llvm::StringRef Name) const;
  const AnalysisBinding *findAnalysis(llvm::StringRef Name) const;

  /// The node an artifact or savepoint of this name resolves to. An empty name
  /// is the pipeline root, which is how the top-level analyses are addressed.
  const PipelineNode *resolveNode(llvm::StringRef Name) const;
};

} // namespace revng::runner
