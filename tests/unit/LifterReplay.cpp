//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE LifterReplay
#include "boost/test/unit_test.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/Lift/AbstractLifter.h"
#include "revng/Loader/AddressSpaceLoader.h"
#include "revng/Model/BinaryIdentifier.h"
#include "revng/Model/Importer/Binary/BinaryImporter.h"
#include "revng/Model/Importer/Binary/Options.h"
#include "revng/Support/InitRevng.h"
#include "revng/UnitTestHelpers/UnitTestHelpers.h"

namespace {

class ImportedAddressSpace final : public revng::loader::AbstractAddressSpace {
private:
  model::Architecture::Values Architecture;
  std::optional<MetaAddress> Entry;
  std::vector<revng::loader::Mapping> Mappings;
  std::vector<MetaAddress> ExtraCode;

public:
  ImportedAddressSpace(const model::Binary &Model,
                       llvm::ArrayRef<uint8_t> Binary) :
    Architecture(Model.Architecture()), Entry(Model.EntryPoint()) {
    for (const model::Segment &Segment : Model.Segments()) {
      const uint64_t EndOffset = Segment.StartOffset() + Segment.FileSize();
      BOOST_REQUIRE(EndOffset <= Binary.size());
      Mappings.push_back({ Segment.StartAddress(),
                           Segment.VirtualSize(),
                           Binary.slice(Segment.StartOffset(),
                                        Segment.StartOffset()
                                          + Segment.FileSize()),
                           Segment.IsReadable(),
                           Segment.IsWriteable(),
                           Segment.IsExecutable(),
                           "imported-segment" });
    }
    ExtraCode.assign(Model.ExtraCodeAddresses().begin(),
                     Model.ExtraCodeAddresses().end());
  }

  model::Architecture::Values architecture() const override {
    return Architecture;
  }
  std::optional<MetaAddress> entryPoint() const override { return Entry; }
  llvm::ArrayRef<revng::loader::Mapping> mappings() const override {
    return Mappings;
  }
  llvm::ArrayRef<MetaAddress> extraCodeAddresses() const override {
    return ExtraCode;
  }
};

std::string printModule(const llvm::Module &Module) {
  std::string Result;
  llvm::raw_string_ostream Stream(Result);
  Module.print(Stream, nullptr);
  return Result;
}

} // namespace

BOOST_AUTO_TEST_CASE(ImportedBinaryAndAddressSpaceReplayMatch) {
  int Argc = 1;
  char ProgramName[] = "test-lifter-replay";
  char *Arguments[] = { ProgramName, nullptr };
  char **Argv = Arguments;
  revng::InitRevng Init(Argc, Argv, "");

  auto BufferOrError = llvm::MemoryBuffer::getFile(REVNG_LIFTER_REPLAY_ELF);
  BOOST_REQUIRE(bool(BufferOrError));
  std::unique_ptr<llvm::MemoryBuffer> Buffer = std::move(*BufferOrError);
  llvm::ArrayRef<uint8_t>
    Bytes(reinterpret_cast<const uint8_t *>(Buffer->getBufferStart()),
          Buffer->getBufferSize());

  TupleTree<model::Binary> Imported;
  model::BinaryIdentifier Identifier;
  Identifier.Index() = 0;
  Identifier.Hash() = std::string(64, '0');
  Identifier.CanonicalPath() = REVNG_LIFTER_REPLAY_ELF;
  Imported->Binaries().insert(std::move(Identifier));
  auto BinaryReference = Imported->getBinaryIdentifierReference(0);
  ImporterOptions Options{ 0, DebugInfoLevel::No, false };
  llvm::cantFail(importBinary(Imported,
                              REVNG_LIFTER_REPLAY_ELF,
                              Options,
                              BinaryReference));

  ImportedAddressSpace AddressSpace(*Imported, Bytes);
  auto Replayed = revng::loader::loadAddressSpace(AddressSpace);
  BOOST_REQUIRE(bool(Replayed));
  if (Imported->EntryPoint().isValid())
    Replayed->Model->Functions()[Imported->EntryPoint()];

  auto OriginalLifter = revng::lift::LifterRegistry::create("libtcg", Imported);
  auto ReplayedLifter = revng::lift::LifterRegistry::create("libtcg",
                                                            Replayed->Model);
  BOOST_REQUIRE(bool(OriginalLifter));
  BOOST_REQUIRE(bool(ReplayedLifter));

  llvm::LLVMContext OriginalContext;
  llvm::LLVMContext ReplayedContext;
  llvm::Module OriginalModule("lifter-replay", OriginalContext);
  llvm::Module ReplayedModule("lifter-replay", ReplayedContext);
  RawBinaryView OriginalView(*Imported, Bytes);
  RawBinaryView ReplayedView = Replayed->view();
  const MetaAddress Entry = Imported->EntryPoint();
  revng::lift::ILifter &Original = **OriginalLifter;
  revng::lift::ILifter &Replay = **ReplayedLifter;
  llvm::cantFail(Original.lift(*Imported,
                               OriginalView,
                               { Entry },
                               OriginalModule));
  llvm::cantFail(Replay.lift(*Replayed->Model,
                             ReplayedView,
                             { Entry },
                             ReplayedModule));

  BOOST_CHECK_EQUAL(printModule(OriginalModule), printModule(ReplayedModule));
}
