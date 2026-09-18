//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <mutex>
#include <set>

#include "revng/Model/TypeDefinition.h"
#include "revng/PipeboxCommon/Helpers/Native/Registry.h"
#include "revng/PipelineC/Manager.h"
#include "revng/Support/Error.h"

namespace revng::embed {

std::optional<ObjectID> objectFromKey(Kind TheKind, llvm::StringRef Key) {
  // A full location path is unambiguous, so take it as given.
  if (Key.starts_with("/")) {
    llvm::Expected<ObjectID> Parsed = ObjectID::deserialize(Key);
    if (not Parsed) {
      llvm::consumeError(Parsed.takeError());
      return std::nullopt;
    }
    if (Parsed->kind() != TheKind)
      return std::nullopt;
    return *Parsed;
  }

  if (TheKind == Kinds::Binary)
    return Key.empty() ? std::optional(ObjectID::root()) : std::nullopt;

  if (TheKind == Kinds::Function) {
    MetaAddress Address = MetaAddress::fromString(Key);
    if (Address.isInvalid())
      return std::nullopt;
    return ObjectID(Address);
  }

  if (TheKind == Kinds::TypeDefinition) {
    auto Parsed = getValueFromYAMLScalar<model::TypeDefinition::Key>(Key);
    return ObjectID(Parsed);
  }

  return std::nullopt;
}

const ContainerHandle *internContainerHandle(ContainerHandle Handle) {
  static std::mutex Mutex;
  static std::set<ContainerHandle> Handles;

  std::lock_guard Guard(Mutex);
  return &*Handles.insert(Handle).first;
}

llvm::Expected<std::unique_ptr<Manager>>
Manager::create(llvm::StringRef PipelinePath) {
  auto Parsed = runner::PipelineDescription::fromFile(PipelinePath);
  if (not Parsed)
    return Parsed.takeError();

  std::unique_ptr<Manager> Result(new Manager());
  Result->Description = std::move(*Parsed);
  Result->TheRunner.emplace(Result->Description, Result->TheModel);
  Result->registerSideTables();
  Result->registerNodes();
  return Result;
}

llvm::Expected<std::unique_ptr<Manager>>
Manager::createFromString(llvm::StringRef PipelineYAML) {
  auto Parsed = runner::PipelineDescription::parse(PipelineYAML);
  if (not Parsed)
    return Parsed.takeError();

  std::unique_ptr<Manager> Result(new Manager());
  Result->Description = std::move(*Parsed);
  Result->TheRunner.emplace(Result->Description, Result->TheModel);
  Result->registerSideTables();
  Result->registerNodes();
  return Result;
}

namespace {

/// Which manager owns which pipeline node. See `Manager::ownerOf`.
std::mutex &ownerMutex() {
  static std::mutex Mutex;
  return Mutex;
}

std::map<const runner::PipelineNode *, Manager *> &owners() {
  static std::map<const runner::PipelineNode *, Manager *> Owners;
  return Owners;
}

} // namespace

void Manager::registerNodes() {
  std::lock_guard Guard(ownerMutex());
  for (const std::unique_ptr<runner::PipelineNode> &Node : Description.Nodes)
    owners()[Node.get()] = this;
}

void Manager::unregisterNodes() {
  std::lock_guard Guard(ownerMutex());
  for (const std::unique_ptr<runner::PipelineNode> &Node : Description.Nodes)
    owners().erase(Node.get());
}

Manager *Manager::ownerOf(const runner::PipelineNode *Node) {
  std::lock_guard Guard(ownerMutex());
  auto It = owners().find(Node);
  return It == owners().end() ? nullptr : It->second;
}

llvm::StringRef Manager::mimeTypeOf(size_t Declaration) {
  using namespace revng::pypeline::helpers::native;

  if (Declaration >= Description.Declarations.size())
    return {};

  llvm::StringRef TypeName = Description.Declarations[Declaration].TypeName;
  if (auto It = MimeTypes.find(TypeName); It != MimeTypes.end())
    return It->second;

  auto It = Registry.Containers.find(TypeName);
  if (It == Registry.Containers.end())
    return {};

  std::string Mime = It->second()->mimeType().str();
  return MimeTypes.insert({ TypeName, std::move(Mime) }).first->second;
}

const std::string &Manager::serializedDescription() {
  if (not SerializedDescription.has_value()) {
    std::string Result;
    llvm::raw_string_ostream Stream(Result);

    Stream << "containers:\n";
    for (const runner::ContainerDeclaration &D : Description.Declarations)
      Stream << "  - name: " << D.Name << "\n    type: " << D.TypeName << "\n";

    Stream << "artifacts:\n";
    for (const auto &Entry : Description.Artifacts)
      Stream << "  - name: " << Entry.first() << "\n    container: "
             << Description.Declarations[Entry.second.Declaration].Name
             << "\n";

    Stream << "analyses:\n";
    for (const auto &Entry : Description.Analyses)
      Stream << "  - name: " << Entry.first() << "\n";

    SerializedDescription = std::move(Result);
  }

  return *SerializedDescription;
}

Manager::~Manager() {
  unregisterNodes();
  unregisterSideTables();
}

void Manager::registerSideTables() {
  const model::Binary *Root = TheModel.get().get();
  if (Root == nullptr)
    return;

  if (Provider != nullptr)
    registerRawBinaryProvider(*Root, Provider);

  if (LifterOverride.has_value()) {
    llvm::Error Failure =
        revng::lift::LifterRegistry::setOverride(*Root, *LifterOverride);
    // The only way this fails is a second override on one model, and this
    // manager is the only thing that registers one against its own root.
    revng_assert(not Failure);
    llvm::consumeError(std::move(Failure));
  }

  RegisteredRoot = Root;
}

void Manager::unregisterSideTables() {
  if (RegisteredRoot == nullptr)
    return;

  unregisterRawBinaryProvider(*RegisteredRoot);
  if (LifterOverride.has_value())
    revng::lift::LifterRegistry::clearOverride(*RegisteredRoot);
  RegisteredRoot = nullptr;
}

void Manager::onModelChanged() {
  ++CommitIndex;

  // `TupleTree` owns its root through a `unique_ptr`, so the address survives a
  // move of the tree but not a wholesale replacement. Anything keyed on it has
  // to be moved across when that happens, or it silently stops applying.
  const model::Binary *Root = TheModel.get().get();
  if (Root != RegisteredRoot) {
    unregisterSideTables();
    registerSideTables();
  }

  // Only what the change touched: a prototype edit leaves the lifted module,
  // which read no prototype, alone.
  if (TheRunner.has_value() and PreImage.has_value()) {
    ModelDiff Diff = PreImage->diff(TheModel);
    if (Diff.size() != 0)
      TheRunner->invalidate(Diff.paths());
  }
  PreImage = TheModel.clone();
}

void Manager::adoptBinaryProvider(std::shared_ptr<RawBinaryProvider> NewProvider) {
  unregisterSideTables();
  Provider = std::move(NewProvider);
  registerSideTables();
}

llvm::Error Manager::adoptLifterFactory(revng::lift::LifterFactory Factory) {
  const model::Binary *Root = TheModel.get().get();
  if (Root == nullptr)
    return revng::createError("the manager has no model to lift");

  if (LifterOverride.has_value())
    revng::lift::LifterRegistry::clearOverride(*Root);
  LifterOverride.emplace(std::move(Factory));
  return revng::lift::LifterRegistry::setOverride(*Root, *LifterOverride);
}

void Manager::releaseBinaryProvider() {
  unregisterSideTables();
  // The manager holds the last reference once the registry's is gone, so the
  // host's release callback runs here rather than at destruction.
  Provider.reset();
  registerSideTables();
}

const ContainerHandle *Manager::internContainer(ContainerHandle Handle) {
  for (const std::unique_ptr<ContainerHandle> &Existing : InternedContainers)
    if (*Existing == Handle)
      return Existing.get();

  InternedContainers.push_back(std::make_unique<ContainerHandle>(Handle));
  return InternedContainers.back().get();
}

} // namespace revng::embed
