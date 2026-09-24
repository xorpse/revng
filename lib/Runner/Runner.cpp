//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <deque>

#include "revng/PipeboxCommon/Helpers/AnalysisRunner.h"
#include "revng/PipeboxCommon/Helpers/Native/Analysis.h"
#include "revng/PipeboxCommon/Helpers/Native/Pipe.h"
#include "revng/PipeboxCommon/Helpers/Native/Registry.h"
#include "revng/Runner/Runner.h"
#include "revng/Support/Debug.h"
#include "revng/Support/Error.h"

using namespace revng::pypeline;
using namespace revng::pypeline::helpers;

namespace revng::runner {

/// Enable with `-debug-log=runner` to see what the scheduler decided to run
/// and why, which is otherwise only inferable from the effects.
static Logger RunnerLog("runner");

namespace {

/// The containers a single schedule run works with.
///
/// Built fresh for every run and destroyed at the end of it, so nothing can
/// accidentally carry one stage's contents into another.
class ContainerSet {
public:
  llvm::Error create(size_t Declaration, llvm::StringRef TypeName) {
    using namespace revng::pypeline::helpers::native;
    auto It = Registry.Containers.find(TypeName);
    if (It == Registry.Containers.end())
      return revng::createError("unregistered container type `" + TypeName.str()
                                + "`");

    Containers.insert_or_assign(Declaration, It->second());
    return llvm::Error::success();
  }

  native::Container *find(size_t Declaration) const {
    auto It = Containers.find(Declaration);
    return It == Containers.end() ? nullptr : It->second.get();
  }

  bool contains(size_t Declaration) const {
    return Containers.count(Declaration) != 0;
  }

private:
  std::map<size_t, std::unique_ptr<native::Container>> Containers;
};

/// Owns the `ObjectID`s a `Request` points at.
///
/// `Request` is a vector of raw pointers, so something has to keep the objects
/// alive for the duration of a call. A deque is used rather than a vector
/// because the addresses must not move as more are added.
class ObjectPool {
public:
  const ObjectID *intern(const ObjectID &Object) {
    Storage.push_back(Object);
    return &Storage.back();
  }

private:
  std::deque<ObjectID> Storage;
};

/// Build the positional `Request` a pipe's `run` expects, in argument order.
Request toRequest(const PipelineNode *Node,
                  const Requests &Wanted,
                  ObjectPool &Pool) {
  Request Result;
  Result.reserve(Node->Arguments.size());

  for (const TaskArgument &Argument : Node->Arguments) {
    std::vector<const ObjectID *> Objects;
    if (const ObjectSet *Set = Wanted.find(Argument.Declaration))
      for (const ObjectID &Object : *Set)
        Objects.push_back(Pool.intern(Object));
    Result.push_back(std::move(Objects));
  }

  return Result;
}

} // namespace

llvm::Expected<Kind> Runner::kindOf(size_t Declaration) const {
  using namespace revng::pypeline::helpers::native;

  if (Declaration >= Description.Declarations.size())
    return revng::createError("container index out of range");

  llvm::StringRef TypeName = Description.Declarations[Declaration].TypeName;
  if (auto It = KindCache.find(TypeName); It != KindCache.end())
    return It->second;

  auto It = Registry.Containers.find(TypeName);
  if (It == Registry.Containers.end())
    return revng::createError("unregistered container type `" + TypeName.str()
                              + "`");

  Kind TheKind = It->second()->kind();
  KindCache.insert({ TypeName, TheKind });
  return TheKind;
}

llvm::Expected<std::set<size_t>> Runner::producedBy(const PipelineNode *Node) {
  using namespace revng::pypeline::helpers::native;

  std::set<size_t> Result;

  // A savepoint can serve anything it caches.
  if (Node->isSavepoint()) {
    for (size_t Declaration : Node->savepoint().ToSave)
      Result.insert(Declaration);
    return Result;
  }

  const PipeTask &ThePipe = Node->pipe();
  auto It = Registry.Pipes.find(ThePipe.PipeName);
  if (It == Registry.Pipes.end())
    return revng::createError("unregistered pipe `" + ThePipe.PipeName + "`");

  std::unique_ptr<native::Pipe> Instance = It->second(ThePipe
                                                        .StaticConfiguration);
  std::vector<native::ContainerArgument> Signature = Instance->signature();

  for (size_t I = 0; I < Node->Arguments.size() and I < Signature.size(); ++I)
    if (Signature[I].Access != Access::Read)
      Result.insert(Node->Arguments[I].Declaration);

  return Result;
}

llvm::Expected<Requests> Runner::prerequisitesFor(const PipelineNode *Node,
                                                  const Requests &Wanted) {
  using namespace revng::pypeline::helpers::native;

  // A savepoint transforms nothing, so what it needs is what was asked of it.
  if (Node->isSavepoint())
    return Wanted;

  const PipeTask &ThePipe = Node->pipe();
  auto It = Registry.Pipes.find(ThePipe.PipeName);
  if (It == Registry.Pipes.end())
    return revng::createError("unregistered pipe `" + ThePipe.PipeName + "`");

  std::unique_ptr<native::Pipe> Instance = It->second(ThePipe
                                                        .StaticConfiguration);
  std::vector<native::ContainerArgument> Signature = Instance->signature();
  if (Signature.size() != Node->Arguments.size())
    return revng::createError("pipe `" + ThePipe.PipeName + "` takes "
                              + std::to_string(Signature.size())
                              + " containers but the pipeline passes "
                              + std::to_string(Node->Arguments.size()));

  Requests Result;

  for (size_t I = 0; I < Node->Arguments.size(); ++I) {
    // A container the pipe only writes is produced, not consumed, so asking
    // for its contents beforehand would schedule work that is about to be
    // overwritten.
    if (Signature[I].Access == Access::Write)
      continue;

    const size_t Declaration = Node->Arguments[I].Declaration;
    llvm::Expected<Kind> TheKind = kindOf(Declaration);
    if (not TheKind)
      return TheKind.takeError();

    ObjectSet Needed(*TheKind);
    for (const auto &[_, Objects] : Wanted)
      Needed.merge(moveToKind(TheModel, Objects, *TheKind));

    if (not Needed.empty())
      Result.merge(Declaration, Needed);
  }

  return Result;
}

llvm::Expected<std::vector<Runner::ScheduledTask>>
Runner::schedule(const PipelineNode *Target, const Requests &Wanted) {
  std::vector<ScheduledTask> Reversed;
  Requests Current = Wanted.minimize();

  for (const PipelineNode *Node = Target;
       Node != nullptr and not Current.empty();
       Node = Node->Predecessor) {
    ScheduledTask Task;
    Task.Node = Node;
    Task.Outgoing = Current;

    // A savepoint already holding what is wanted ends the walk: everything
    // before it can be skipped and the contents loaded instead.
    if (Node->isSavepoint()) {
      Requests StillNeeded;
      for (const auto &[Declaration, Objects] : Current) {
        ObjectSet Missing = Storage.missing({ Node->Id, Declaration }, Objects);
        if (not Missing.empty())
          StillNeeded.merge(Declaration, Missing);
      }

      if (StillNeeded.empty()) {
        Task.Incoming = Requests();
        Reversed.push_back(std::move(Task));
        break;
      }

      // Only what is missing travels back; `Task.Outgoing` keeps the full
      // set so the cached remainder is loaded from storage.
      Current = std::move(StillNeeded);
    }

    llvm::Expected<std::set<size_t>> Produced = producedBy(Node);
    if (not Produced)
      return Produced.takeError();

    // Writes nothing still wanted, so neither it nor its inputs are needed.
    if (not Node->isSavepoint()) {
      bool Contributes = false;
      for (size_t Declaration : *Produced)
        if (Current.find(Declaration) != nullptr)
          Contributes = true;

      if (not Contributes)
        continue;
    }

    llvm::Expected<Requests> Incoming = prerequisitesFor(Node, Current);
    if (not Incoming)
      return Incoming.takeError();

    Task.Incoming = *Incoming;

    // A request for a container this node does not produce has to keep
    // travelling: `simplify-switch` reads `binaries-container`, which nothing
    // between it and `import-files` touches, so dropping it here would leave
    // that container empty by the time the pipe runs.
    Requests Next = *Incoming;
    for (const auto &[Declaration, Objects] : Current)
      if (Produced->count(Declaration) == 0)
        Next.merge(Declaration, Objects);

    Reversed.push_back(std::move(Task));
    Current = Next.minimize();
  }

  std::reverse(Reversed.begin(), Reversed.end());
  if (RunnerLog.isEnabled()) {
    revng_log(RunnerLog, "schedule:");
    LoggerIndent Indent(RunnerLog);
    for (const ScheduledTask &Task : Reversed) {
      std::string Line = Task.Node->name().str();
      if (Task.Node->isSavepoint())
        Line += " (savepoint)";
      Line += " reads {";
      for (const auto &[Declaration, Objects] : Task.Incoming)
        Line += Description.Declarations[Declaration].Name + ":"
                + std::to_string(Objects.size()) + " ";
      Line += "} writes {";
      for (const auto &[Declaration, Objects] : Task.Outgoing)
        Line += Description.Declarations[Declaration].Name + ":"
                + std::to_string(Objects.size()) + " ";
      Line += "}";
      revng_log(RunnerLog, Line);
    }
  }

  return Reversed;
}

llvm::Error Runner::runSchedule(llvm::ArrayRef<ScheduledTask> Tasks) {
  using namespace revng::pypeline::helpers::native;

  ContainerSet Containers;

  // Every container any task touches, created once for this run only.
  for (const ScheduledTask &Task : Tasks) {
    for (const TaskArgument &Argument : Task.Node->Arguments) {
      if (Containers.contains(Argument.Declaration))
        continue;
      llvm::StringRef TypeName = Description.Declarations[Argument.Declaration]
                                   .TypeName;
      if (llvm::Error Error = Containers.create(Argument.Declaration, TypeName))
        return Error;
    }
  }

  for (const ScheduledTask &Task : Tasks) {
    const PipelineNode *Node = Task.Node;

    if (Node->isSavepoint()) {
      // Load whatever is cached here, then store back whatever the run just
      // produced, so a later request can start from this point.
      for (size_t Declaration : Node->savepoint().ToSave) {
        native::Container *Container = Containers.find(Declaration);
        if (Container == nullptr)
          continue;

        ContainerLocation Location{ Node->Id, Declaration };

        if (const ObjectSet *Wanted = Task.Outgoing.find(Declaration)) {
          std::set<ObjectID> Cached = Storage.has(Location, *Wanted);
          if (not Cached.empty()) {
            ObjectSet ToLoad(Wanted->kind(), Cached);
            Container->deserialize(Storage.get(Location, ToLoad));
          }
        }

        std::set<ObjectID> Present = Container->objects();
        std::set<ObjectID> Known = Storage.objectsAt(Location);
        std::vector<ObjectID> ToSave;
        for (const ObjectID &Object : Present)
          if (Known.count(Object) == 0)
            ToSave.push_back(Object);

        if (not ToSave.empty())
          Storage.add(Location, Container->serialize(ToSave));
      }

      continue;
    }

    const PipeTask &ThePipe = Node->pipe();
    auto It = Registry.Pipes.find(ThePipe.PipeName);
    if (It == Registry.Pipes.end())
      return revng::createError("unregistered pipe `" + ThePipe.PipeName + "`");

    std::unique_ptr<native::Pipe> Instance = It->second(ThePipe
                                                          .StaticConfiguration);
    ++PipesRun;

    // The pipe casts each container to the type it expects, positionally and
    // without checking, so a mismatch is a bad cast rather than an error.
    std::vector<native::ContainerArgument> Signature = Instance->signature();
    if (Signature.size() != Node->Arguments.size())
      return revng::createError("pipe `" + ThePipe.PipeName + "` takes "
                                + std::to_string(Signature.size())
                                + " containers but the pipeline passes "
                                + std::to_string(Node->Arguments.size()));

    std::vector<native::Container *> Arguments;
    for (size_t I = 0; I < Node->Arguments.size(); ++I) {
      const size_t Declaration = Node->Arguments[I].Declaration;
      llvm::StringRef Declared = Description.Declarations[Declaration].TypeName;
      if (Declared != Signature[I].ContainerTypeName)
        return revng::createError("pipe `" + ThePipe.PipeName + "` expects a "
                                  + Signature[I].ContainerTypeName.str()
                                  + " as argument " + std::to_string(I)
                                  + " but the pipeline passes `"
                                  + Description.Declarations[Declaration].Name
                                  + "`, which is a " + Declared.str());

      native::Container *Container = Containers.find(Declaration);
      revng_assert(Container != nullptr);
      Arguments.push_back(Container);
    }

    ObjectPool Pool;
    Request Incoming = toRequest(Node, Task.Incoming, Pool);
    Request Outgoing = toRequest(Node, Task.Outgoing, Pool);

    llvm::StringRef Dynamic;
    if (auto Found = DynamicConfiguration.find(ThePipe.PipeName);
        Found != DynamicConfiguration.end())
      Dynamic = Found->second;

    // Reference caching makes repeated model lookups cheap, and a pipe is not
    // allowed to mutate the model, so it is safe for the duration of the run.
    TheModel.enableCaching();
    PipeOutput Output = Instance->run(TheModel,
                                      Arguments,
                                      Incoming,
                                      Outgoing,
                                      Dynamic);
    TheModel.disableCaching();

    for (size_t I = 0; I < Output.Dependencies.size(); ++I) {
      const size_t Declaration = Node->Arguments[I].Declaration;
      for (const auto &[Object, Path] : Output.Dependencies[I])
        Storage.dependOn(Path,
                         DependencyEntry{ Node->Downstream.Start,
                                          Node->Downstream.End,
                                          Declaration,
                                          Object });
    }
  }

  return llvm::Error::success();
}

llvm::Error Runner::produce(const PipelineNode *Target, const Requests &Wanted) {
  if (Target == nullptr)
    return revng::createError("no such step in the pipeline");

  llvm::Expected<std::vector<ScheduledTask>> Tasks = schedule(Target, Wanted);
  if (not Tasks)
    return Tasks.takeError();

  return runSchedule(*Tasks);
}

llvm::Expected<revng::pypeline::Buffer>
Runner::produceOne(const PipelineNode *Target,
                   size_t Declaration,
                   const ObjectID &Object) {
  llvm::Expected<Kind> TheKind = kindOf(Declaration);
  if (not TheKind)
    return TheKind.takeError();

  if (Object.kind() != *TheKind)
    return revng::createError("the requested object does not match the "
                              "container's granularity");

  Requests Wanted;
  Wanted.set(Declaration, ObjectSet(*TheKind, { Object }));

  if (llvm::Error Error = produce(Target, Wanted))
    return std::move(Error);

  // `produce` leaves the result in storage only when it crossed a savepoint,
  // so re-run against a set that includes this node to read it back.
  const PipelineNode *Node = Target;
  while (Node != nullptr and not Node->isSavepoint())
    Node = Node->Predecessor;

  if (Node == nullptr)
    return revng::createError("no savepoint holds the requested object");

  ContainerLocation Location{ Node->Id, Declaration };
  ObjectSet One(*TheKind, { Object });
  auto Stored = Storage.get(Location, One);
  if (Stored.empty())
    return revng::createError("the pipeline produced no data for the requested "
                              "object");

  revng::pypeline::Buffer Result;
  llvm::ArrayRef<char> Data = Stored.begin()->second;
  Result.data().assign(Data.begin(), Data.end());
  return Result;
}

llvm::Expected<revng::pypeline::Buffer>
Runner::produceArtifact(llvm::StringRef ArtifactName, const ObjectID &Object) {
  const Artifact *TheArtifact = Description.findArtifact(ArtifactName);
  if (TheArtifact == nullptr)
    return revng::createError("no such artifact `" + ArtifactName.str() + "`");

  return produceOne(TheArtifact->Node, TheArtifact->Declaration, Object);
}

llvm::Error Runner::runAnalysis(llvm::StringRef Name,
                                llvm::StringRef ConfigurationYAML) {
  using namespace revng::pypeline::helpers::native;

  const AnalysisBinding *Binding = Description.findAnalysis(Name);
  if (Binding == nullptr)
    return revng::createError("no such analysis `" + Name.str() + "`");

  auto It = Registry.Analyses.find(Name);
  if (It == Registry.Analyses.end())
    return revng::createError("unregistered analysis `" + Name.str() + "`");

  // The analysis reads containers at the node it is bound to, so make sure
  // they are there first.
  Requests Wanted;
  for (size_t Declaration : Binding->Arguments) {
    llvm::Expected<Kind> TheKind = kindOf(Declaration);
    if (not TheKind)
      return TheKind.takeError();

    ObjectSet All(*TheKind);
    if (*TheKind == Kinds::Binary) {
      All.insert(ObjectID::root());
    } else {
      for (const ObjectID &Child : TheModel.children(ObjectID::root(), *TheKind))
        All.insert(Child);
    }
    Wanted.merge(Declaration, All);
  }

  if (Binding->Node != nullptr)
    if (llvm::Error Error = produce(Binding->Node, Wanted))
      return Error;

  ContainerSet Containers;
  std::vector<native::Container *> Arguments;
  for (size_t Declaration : Binding->Arguments) {
    llvm::StringRef TypeName = Description.Declarations[Declaration].TypeName;
    if (llvm::Error Error = Containers.create(Declaration, TypeName))
      return Error;

    native::Container *Container = Containers.find(Declaration);
    if (Binding->Node != nullptr) {
      const PipelineNode *Node = Binding->Node;
      while (Node != nullptr
             and (not Node->isSavepoint()
                  or not llvm::is_contained(Node->savepoint().ToSave,
                                            Declaration)))
        Node = Node->Predecessor;

      if (Node != nullptr) {
        ContainerLocation Location{ Node->Id, Declaration };
        if (const ObjectSet *Set = Wanted.find(Declaration)) {
          auto Stored = Storage.get(Location, *Set);
          // Analyses index their containers without checking, so an empty one
          // crashes somewhere inside rather than failing here.
          if (Stored.empty())
            return revng::createError(
              "analysis `" + Name.str() + "` reads container `"
              + Description.Declarations[Declaration].Name
              + "`, which holds nothing at savepoint `"
              + Node->savepoint().Name + "`");

          Container->deserialize(Stored);
        }
      }
    }
    Arguments.push_back(Container);
  }

  ObjectPool Pool;
  Request Incoming;
  for (size_t Declaration : Binding->Arguments) {
    std::vector<const ObjectID *> Objects;
    if (const ObjectSet *Set = Wanted.find(Declaration))
      for (const ObjectID &Object : *Set)
        Objects.push_back(Pool.intern(Object));
    Incoming.push_back(std::move(Objects));
  }

  std::unique_ptr<native::Analysis> Instance = It->second();

  // An analysis mutates the model, so reference caching has to be off: cached
  // references would survive the edit and point at the old tree.
  TheModel.disableCaching();
  llvm::Error Result = Instance->run(TheModel,
                                     Arguments,
                                     Incoming,
                                     ConfigurationYAML);
  if (Result)
    return Result;

  return llvm::Error::success();
}

llvm::Error Runner::runAnalysisList(llvm::StringRef Name,
                                    llvm::StringRef ConfigurationYAML) {
  auto It = Description.AnalysisLists.find(Name);
  if (It == Description.AnalysisLists.end())
    return revng::createError("no such analysis list `" + Name.str() + "`");

  for (const std::string &Analysis : It->second)
    if (llvm::Error Error = runAnalysis(Analysis, ConfigurationYAML))
      return Error;

  return llvm::Error::success();
}

} // namespace revng::runner
