//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/PipeboxCommon/Helpers/Native/Registry.h"
#include "revng/Runner/PipelineDescription.h"
#include "revng/Support/Error.h"

using namespace revng::runner;

namespace {

/// A materialised YAML value.
///
/// `llvm::yaml::Stream` parses lazily and each node can only be walked once,
/// which does not suit a description that is queried by key several times. Read
/// the document into this in a single pass and query it afterwards.
struct Value {
  enum class Kind { Scalar, Sequence, Mapping } TheKind = Kind::Scalar;
  std::string Scalar;
  std::vector<Value> Sequence;
  std::vector<std::pair<std::string, Value>> Mapping;

  bool isScalar() const { return TheKind == Kind::Scalar; }
  bool isSequence() const { return TheKind == Kind::Sequence; }
  bool isMapping() const { return TheKind == Kind::Mapping; }

  /// The value for `Key`, or nullptr. Duplicate keys are not expected and the
  /// first wins, matching the YAML parsers used elsewhere.
  const Value *find(llvm::StringRef Key) const {
    if (not isMapping())
      return nullptr;
    for (const auto &[Name, Child] : Mapping)
      if (Name == Key)
        return &Child;
    return nullptr;
  }

  std::string string(llvm::StringRef Key) const {
    const Value *Child = find(Key);
    return (Child != nullptr and Child->isScalar()) ? Child->Scalar
                                                    : std::string();
  }

  std::vector<std::string> strings(llvm::StringRef Key) const {
    std::vector<std::string> Result;
    const Value *Child = find(Key);
    if (Child != nullptr and Child->isSequence())
      for (const Value &Element : Child->Sequence)
        if (Element.isScalar())
          Result.push_back(Element.Scalar);
    return Result;
  }

  /// Re-emit as YAML. Used for a pipe's `configuration`, which the schema
  /// allows to be either a scalar or a mapping and which is handed to the pipe
  /// as a string either way.
  std::string reserialize() const {
    if (isScalar())
      return Scalar;

    std::string Result;
    llvm::raw_string_ostream Stream(Result);
    if (isMapping()) {
      for (const auto &[Name, Child] : Mapping) {
        if (Child.isSequence()) {
          Stream << Name << ":\n";
          for (const Value &Element : Child.Sequence)
            Stream << "  - " << Element.Scalar << "\n";
        } else {
          Stream << Name << ": " << Child.Scalar << "\n";
        }
      }
    }
    return Result;
  }
};

Value materialize(llvm::yaml::Node *Node) {
  Value Result;

  if (auto *Scalar = llvm::dyn_cast_or_null<llvm::yaml::ScalarNode>(Node)) {
    llvm::SmallString<64> Storage;
    Result.TheKind = Value::Kind::Scalar;
    Result.Scalar = Scalar->getValue(Storage).str();
  } else if (auto *Block =
               llvm::dyn_cast_or_null<llvm::yaml::BlockScalarNode>(Node)) {
    Result.TheKind = Value::Kind::Scalar;
    Result.Scalar = Block->getValue().str();
  } else if (auto *Sequence =
               llvm::dyn_cast_or_null<llvm::yaml::SequenceNode>(Node)) {
    Result.TheKind = Value::Kind::Sequence;
    for (llvm::yaml::Node &Element : *Sequence)
      Result.Sequence.push_back(materialize(&Element));
  } else if (auto *Mapping =
               llvm::dyn_cast_or_null<llvm::yaml::MappingNode>(Node)) {
    Result.TheKind = Value::Kind::Mapping;
    for (llvm::yaml::KeyValueNode &Entry : *Mapping) {
      llvm::SmallString<32> Storage;
      auto *Key = llvm::dyn_cast_or_null<llvm::yaml::ScalarNode>(Entry.getKey());
      if (Key == nullptr)
        continue;
      Result.Mapping.emplace_back(Key->getValue(Storage).str(),
                                  materialize(Entry.getValue()));
    }
  }

  return Result;
}

} // namespace

std::optional<size_t>
PipelineDescription::findDeclaration(llvm::StringRef Name) const {
  for (size_t I = 0; I < Declarations.size(); ++I)
    if (Declarations[I].Name == Name)
      return I;
  return std::nullopt;
}

const PipelineNode *
PipelineDescription::findSavepoint(llvm::StringRef Name) const {
  for (const std::unique_ptr<PipelineNode> &Node : Nodes)
    if (Node->isSavepoint() and Node->savepoint().Name == Name)
      return Node.get();
  return nullptr;
}

const Artifact *PipelineDescription::findArtifact(llvm::StringRef Name) const {
  auto It = Artifacts.find(Name);
  return It == Artifacts.end() ? nullptr : &It->second;
}

const AnalysisBinding *
PipelineDescription::findAnalysis(llvm::StringRef Name) const {
  auto It = Analyses.find(Name);
  return It == Analyses.end() ? nullptr : &It->second;
}

const PipelineNode *
PipelineDescription::resolveNode(llvm::StringRef Name) const {
  if (Name.empty())
    return nullptr;

  if (const PipelineNode *Node = findSavepoint(Name))
    return Node;

  if (const Artifact *TheArtifact = findArtifact(Name))
    return TheArtifact->Node;

  return nullptr;
}

llvm::Expected<PipelineDescription>
PipelineDescription::parse(llvm::StringRef YAML) {
  llvm::SourceMgr SourceManager;
  llvm::yaml::Stream Stream(YAML, SourceManager);
  auto It = Stream.begin();
  if (It == Stream.end())
    return revng::createError("pipeline description is empty");

  Value Document = materialize(It->getRoot());
  if (not Document.isMapping())
    return revng::createError("pipeline description is not a YAML mapping");

  PipelineDescription Result;

  const Value *Containers = Document.find("containers");
  if (Containers == nullptr or not Containers->isSequence())
    return revng::createError("pipeline description has no `containers`");

  for (const Value &Entry : Containers->Sequence) {
    ContainerDeclaration Declaration{ Entry.string("name"),
                                      Entry.string("type"),
                                      Result.Declarations.size() };
    if (Declaration.Name.empty() or Declaration.TypeName.empty())
      return revng::createError("a container declaration is missing its name "
                                "or type");
    Result.Declarations.push_back(std::move(Declaration));
  }

  if (const Value *Lists = Document.find("analysis-lists");
      Lists != nullptr and Lists->isSequence()) {
    for (const Value &Entry : Lists->Sequence) {
      std::string Name = Entry.string("name");
      if (not Name.empty())
        Result.AnalysisLists[Name] = Entry.strings("analyses");
    }
  }

  // Analyses declared at the top level read no container, so they are bound to
  // the pipeline root rather than to a node.
  if (const Value *RootAnalyses = Document.find("analyses");
      RootAnalyses != nullptr and RootAnalyses->isSequence()) {
    for (const Value &Entry : RootAnalyses->Sequence) {
      AnalysisBinding Binding;
      Binding.Name = Entry.string("analysis");
      Binding.Description = Entry.string("description");
      if (not Binding.Name.empty())
        Result.Analyses[Binding.Name] = std::move(Binding);
    }
  }

  const Value *Branches = Document.find("branches");
  if (Branches == nullptr or not Branches->isMapping())
    return revng::createError("pipeline description has no `branches`");

  auto declarationIndex =
    [&Result](llvm::StringRef Name) -> llvm::Expected<size_t> {
    if (std::optional<size_t> Index = Result.findDeclaration(Name))
      return *Index;
    return revng::createError("unknown container `" + Name.str() + "`");
  };

  // A branch may continue one declared later in the document, so build on
  // demand and memoise. The `from` relation is a tree, so this terminates.
  struct PendingBranch {
    const Value *Body = nullptr;
    PipelineNode *Last = nullptr;
    bool Built = false;
  };
  llvm::StringMap<PendingBranch> Pending;
  for (const auto &[Name, Body] : Branches->Mapping)
    Pending[Name] = PendingBranch{ &Body, nullptr, false };

  std::function<llvm::Error(llvm::StringRef)> build =
    [&](llvm::StringRef Name) -> llvm::Error {
    auto It = Pending.find(Name);
    if (It == Pending.end())
      return revng::createError("unknown branch `" + Name.str() + "`");

    PendingBranch &Branch = It->second;
    if (Branch.Built)
      return llvm::Error::success();
    // Mark as built up front: a cycle would otherwise recurse forever, and the
    // resulting tree is checked by the caller.
    Branch.Built = true;

    const Value *Tasks = Branch.Body->find("tasks");
    if (Tasks == nullptr or not Tasks->isSequence())
      return revng::createError("branch `" + Name.str() + "` has no `tasks`");

    PipelineNode *Previous = nullptr;
    std::string From = Branch.Body->string("from");
    if (not From.empty()) {
      if (llvm::Error Error = build(From))
        return Error;
      Previous = Pending[From].Last;
    }

    for (const Value &Task : Tasks->Sequence) {
      auto Node = std::make_unique<PipelineNode>();

      std::string PipeName = Task.string("pipe");
      std::string SavepointName = Task.string("savepoint");
      std::vector<std::string> ArgumentNames;

      if (not PipeName.empty()) {
        PipeTask ThePipe;
        ThePipe.PipeName = std::move(PipeName);
        if (const Value *Configuration = Task.find("configuration"))
          ThePipe.StaticConfiguration = Configuration->reserialize();
        Node->Task = std::move(ThePipe);
        ArgumentNames = Task.strings("arguments");
      } else if (not SavepointName.empty()) {
        SavepointTask TheSavepoint;
        TheSavepoint.Name = std::move(SavepointName);
        Node->Task = std::move(TheSavepoint);
        ArgumentNames = Task.strings("containers");
      } else {
        return revng::createError("a task in branch `" + Name.str()
                                  + "` is neither a pipe nor a savepoint");
      }

      for (const std::string &ArgumentName : ArgumentNames) {
        llvm::Expected<size_t> Index = declarationIndex(ArgumentName);
        if (not Index)
          return Index.takeError();
        Node->Arguments.push_back({ *Index, revng::pypeline::Access::Auto });
      }

      if (Node->isSavepoint()) {
        SavepointTask &TheSavepoint = std::get<SavepointTask>(Node->Task);
        for (const TaskArgument &Argument : Node->Arguments)
          TheSavepoint.ToSave.push_back(Argument.Declaration);
      }

      Node->Predecessor = Previous;
      PipelineNode *Raw = Node.get();
      if (Previous != nullptr)
        Previous->Successors.push_back(Raw);
      Result.Nodes.push_back(std::move(Node));
      Previous = Raw;

      // Artifacts and analyses attach to the task they are declared under.
      if (const Value *Artifacts = Task.find("artifacts");
          Artifacts != nullptr and Artifacts->isSequence()) {
        for (const Value &Entry : Artifacts->Sequence) {
          Artifact TheArtifact;
          TheArtifact.Name = Entry.string("name");
          TheArtifact.Description = Entry.string("description");
          TheArtifact.Category = Entry.string("category");
          TheArtifact.Filename = Entry.string("filename");
          TheArtifact.Node = Raw;

          llvm::Expected<size_t> Index = declarationIndex(Entry
                                                            .string("containe"
                                                                    "r"));
          if (not Index)
            return Index.takeError();
          TheArtifact.Declaration = *Index;

          if (not TheArtifact.Name.empty())
            Result.Artifacts[TheArtifact.Name] = std::move(TheArtifact);
        }
      }

      if (const Value *Analyses = Task.find("analyses");
          Analyses != nullptr and Analyses->isSequence()) {
        for (const Value &Entry : Analyses->Sequence) {
          AnalysisBinding Binding;
          Binding.Name = Entry.string("analysis");
          Binding.Description = Entry.string("description");
          Binding.Node = Raw;

          for (const std::string &ContainerName : Entry.strings("containers")) {
            llvm::Expected<size_t> Index = declarationIndex(ContainerName);
            if (not Index)
              return Index.takeError();
            Binding.Arguments.push_back(*Index);
          }

          if (not Binding.Name.empty())
            Result.Analyses[Binding.Name] = std::move(Binding);
        }
      }
    }

    Branch.Last = Previous;
    return llvm::Error::success();
  };

  for (const auto &[Name, Body] : Branches->Mapping)
    if (llvm::Error Error = build(Name))
      return std::move(Error);

  Result.assignSavepointRanges();
  return Result;
}

void PipelineDescription::assignSavepointRanges() {
  size_t Next = 0;
  auto Walk = [&Next](auto &Self, PipelineNode *Node) -> void {
    Node->Downstream.Start = Next;
    if (Node->isSavepoint())
      Node->Id = Next++;

    for (PipelineNode *Successor : Node->Successors)
      Self(Self, Successor);

    Node->Downstream.End = Next == Node->Downstream.Start ? Next : Next - 1;
  };

  for (const std::unique_ptr<PipelineNode> &Node : Nodes)
    if (Node->Predecessor == nullptr)
      Walk(Walk, Node.get());
}

llvm::Expected<PipelineDescription>
PipelineDescription::fromFile(llvm::StringRef Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path);
  if (not Buffer)
    return revng::createError("cannot read pipeline description at `"
                              + Path.str() + "`");
  return parse((*Buffer)->getBuffer());
}
