//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Lift/AbstractLifter.h"
#include "revng/Lift/LibTcg.h"
#include "revng/Support/ResourceFinder.h"

#include "CodeGenerator.h"
#include "LibTcgLifter.h"

namespace revng::lift {
namespace {

struct ExternalFilePaths {
  std::string LibHelpers;
  std::string EarlyLinked;
};

ExternalFilePaths
findExternalFilePaths(model::Architecture::Values Architecture) {
  const std::string ArchName = model::Architecture::getQEMUName(Architecture)
                                 .str();
  ExternalFilePaths Paths;
  auto Helpers = ResourceFinder.findFile("/share/revng/"
                                         "libtcg-helpers-declarations-only-"
                                         + ArchName + ".bc");
  if (not Helpers)
    revng_abort("Cannot find libtcg helpers");
  Paths.LibHelpers = *Helpers;

  auto EarlyLinked = ResourceFinder.findFile("/share/revng/early-linked-"
                                             + ArchName + ".ll");
  if (not EarlyLinked)
    revng_abort("Cannot find early-linked.ll");
  Paths.EarlyLinked = *EarlyLinked;
  return Paths;
}

class LibTcgLifter final : public ILifter {
private:
  const TupleTree<model::Binary> &Model;

public:
  explicit LibTcgLifter(const TupleTree<model::Binary> &Model) : Model(Model) {}

  llvm::Error lift(const model::Binary &Binary,
                   const RawBinaryView &View,
                   llvm::ArrayRef<MetaAddress> Entries,
                   llvm::Module &Output) override {
    const ExternalFilePaths Paths = findExternalFilePaths(Binary
                                                            .Architecture());
    LibTcg TheLibTcg = LibTcg::get(Binary.Architecture());
    CodeGenerator Generator(View,
                            &Output,
                            Model,
                            Paths.LibHelpers,
                            Paths.EarlyLinked,
                            Binary.Architecture());

    std::optional<uint64_t> Entry;
    if (not Entries.empty())
      Entry = Entries.front().address();
    Generator.translate(TheLibTcg, Entry);
    return llvm::Error::success();
  }
};

} // namespace

std::unique_ptr<ILifter>
createLibTcgLifter(const TupleTree<model::Binary> &Model) {
  return std::make_unique<LibTcgLifter>(Model);
}

} // namespace revng::lift
