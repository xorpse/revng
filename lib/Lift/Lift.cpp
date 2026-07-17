//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/Support/Progress.h"

#include "revng/Lift/AbstractLifter.h"
#include "revng/Lift/Lift.h"
#include "revng/Lift/PostLiftVerifyPass.h"
#include "revng/Support/CommandLine.h"
#include "revng/Support/IRHelpers.h"

using namespace llvm::cl;

namespace {
const char *EntryDescStr = "virtual address of the entry point where to start";
opt<unsigned long long> EntryPointAddress("entry",
                                          desc(EntryDescStr),
                                          value_desc("address"),
                                          cat(MainCategory));
alias A1("e",
         desc("Alias for -entry"),
         aliasopt(EntryPointAddress),
         cat(MainCategory));
opt<std::string> LifterBackend("lifter-backend",
                               desc("lifter backend to use"),
                               value_desc("name"),
                               init(""),
                               cat(MainCategory));

} // namespace

char LiftPass::ID;

using Register = llvm::RegisterPass<LiftPass>;
static Register X("lift", "Lift Pass", true, true);

bool LiftPass::runOnModule(llvm::Module &M) {
  llvm::Task T(2, "Lift pass");
  const auto &ModelWrapper = getAnalysis<LoadModelWrapperPass>().get();
  const TupleTree<model::Binary> &Model = ModelWrapper.getReadOnlyModel();

  // Get access to raw binary data
  RawBinaryView &RawBinary = getAnalysis<LoadBinaryWrapperPass>().get();
  auto MaybeLifter = LifterBackend.empty() ?
                       revng::lift::LifterRegistry::createDefault(Model) :
                       revng::lift::LifterRegistry::create(LifterBackend,
                                                           Model);
  auto Lifter = llvm::cantFail(std::move(MaybeLifter));
  llvm::SmallVector<MetaAddress, 1> Entries;
  if (EntryPointAddress.getNumOccurrences() != 0)
    Entries.push_back(MetaAddress::fromPC(Model->Architecture(),
                                          EntryPointAddress));
  T.advance("Translate", true);
  llvm::cantFail(Lifter->lift(*Model, RawBinary, Entries, M));

  sortModule(M);

  return false;
}

/// Map describing the jump targets in the LLVM module, each one is identified
/// by its MetaAddress. The boolean value represents if the jump target has been
/// discovered through harvesting (false) or successively through the list of
/// model functions (true).
using JumpTargetMap = std::map<MetaAddress, bool>;

template<>
struct llvm::yaml::CustomMappingTraits<JumpTargetMap> {
  static void inputOne(IO &IO, StringRef Key, JumpTargetMap &Data) {
    MetaAddress Address = MetaAddress::fromString(Key);
    IO.mapRequired(Key.str().c_str(), Data[Address]);
  }

  static void output(IO &IO, JumpTargetMap &Data) {
    for (auto &[Key, Value] : Data)
      IO.mapRequired(Key.toString().c_str(), Value);
  }
};

namespace revng::pypeline::piperuns {

Lift::Lift(const class Model &Model,
           llvm::StringRef Config,
           llvm::StringRef DynamicConfig,
           const BinariesContainer &Binary,
           LLVMRootContainer &ModuleContainer) :
  TheModel(Model), Binary(Binary), ModuleContainer(ModuleContainer) {
}

CustomInvalidationData Lift::run() {
  llvm::Task T(4, "Lift");
  const TupleTree<model::Binary> &Model = TheModel.get();

  // Get access to raw binary data
  revng_assert(Binary.size() == 1);
  llvm::ArrayRef<char> File = Binary.getFile(0);
  RawBinaryView RawBinary(*Model, { File.data(), File.size() });
  llvm::Module &Module = ModuleContainer.getModule();

  auto Lifter = llvm::cantFail(LifterBackend.empty() ?
                                 lift::LifterRegistry::createDefault(Model) :
                                 lift::LifterRegistry::create(LifterBackend,
                                                              Model));
  llvm::SmallVector<MetaAddress, 1> Entries;
  if (EntryPointAddress.getNumOccurrences() != 0)
    Entries.push_back(MetaAddress::fromPC(Model->Architecture(),
                                          EntryPointAddress));
  T.advance("Translate", true);
  llvm::cantFail(Lifter->lift(*Model, RawBinary, Entries, Module));

  T.advance("Sort Module", true);
  sortModule(Module);

  T.advance("Verify Module", true);
  // TODO: convert this from a pass to a free-standing function
  PostLiftVerifyPass{}.runOnModule(Module);

  // TODO: substitute with strip-dead-debug-info once the old pipeline
  //       is dropped
  pruneDICompileUnits(Module);

  // Compute invalidation data
  Buffer SerializedInvalidation;

  {
    auto [HasRoot, JumpTargets] = lift::internal::collectJumpTargets(Module);
    revng_assert(HasRoot);
    llvm::raw_svector_ostream OS(SerializedInvalidation.data());
    serialize(OS, JumpTargets);
  }

  return { {}, { { ObjectID(), SerializedInvalidation } } };
}

llvm::Error Lift::checkPrecondition(const class Model &Model) {
  const model::Binary &Binary = *Model.get().get();
  llvm::Error BackendError = llvm::Error::success();
  if (not lift::LifterRegistry::hasLifter(Binary)) {
    std::string Message = "no lifter backend is registered for ";
    Message += model::Architecture::getName(Binary.Architecture());
    BackendError = revng::createError(Message);
  }
  return revng::joinErrors(std::move(BackendError),
                           lift::internal::checkPrecondition(Binary),
                           RawBinaryView::checkPrecondition(Binary));
}

bool Lift::requiresCustomInvalidation(const ModelDiff &Diff) {
  using Fields = TupleLikeTraits<model::Binary>::Fields;
  size_t FunctionsIndex = static_cast<size_t>(Fields::Functions);
  for (const auto &Change : Diff.get().Changes) {
    if (Change.Path.size() == 1
        and Change.Path[0].get<size_t>() == FunctionsIndex)
      return true;
  }
  return false;
}

std::vector<std::set<ObjectID>>
Lift::processCustomInvalidation(const InvalidationData &Data,
                                const ModelDiff &Diff) {
  auto LLVMModuleData = Data.at(1);
  revng_assert(LLVMModuleData.size() == 1);
  revng_assert(*std::get<0>(LLVMModuleData[0]) == ObjectID());

  auto DataBuffer = std::get<1>(LLVMModuleData[0]);
  llvm::StringRef String(reinterpret_cast<const char *>(DataBuffer.data()),
                         DataBuffer.size());
  auto JumpTargets = llvm::cantFail(fromString<JumpTargetMap>(String));

  if (lift::internal::shouldInvalidateRoot(JumpTargets, Diff.get()))
    return { {}, { ObjectID() } };
  else
    return {};
}

} // namespace revng::pypeline::piperuns
