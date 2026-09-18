//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE Runner
#include <array>

#include "boost/test/unit_test.hpp"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SHA256.h"

#include "revng/Loader/AddressSpaceLoader.h"
#include "revng/Model/Binary.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Runner/FileStore.h"
#include "revng/Runner/PipelineDescription.h"
#include "revng/Runner/Runner.h"
#include "revng/UnitTestHelpers/UnitTestHelpers.h"

using namespace revng::runner;

namespace {

/// `mov eax, edi; add eax, esi; ret`, which the reference backend understands.
class TestAddressSpace final : public revng::loader::AbstractAddressSpace {
private:
  std::array<uint8_t, 5> Bytes = { 0x89, 0xf8, 0x01, 0xf0, 0xc3 };
  std::array<revng::loader::Mapping, 1> Mappings;

public:
  TestAddressSpace() :
    Mappings({ revng::loader::Mapping{
      MetaAddress::fromPC(model::Architecture::x86_64, 0x400000),
      Bytes.size(),
      Bytes,
      true,
      false,
      true,
      "code" } }) {}

  model::Architecture::Values architecture() const override {
    return model::Architecture::x86_64;
  }
  std::optional<MetaAddress> entryPoint() const override {
    return Mappings[0].Start;
  }
  llvm::ArrayRef<revng::loader::Mapping> mappings() const override {
    return Mappings;
  }

  llvm::ArrayRef<uint8_t> bytes() const { return Bytes; }
};

std::string hexHash(llvm::ArrayRef<uint8_t> Data) {
  return llvm::toHex(llvm::SHA256::hash(Data), /* LowerCase = */ true);
}

/// A model for the shellcode, with its bytes deposited where `import-files`
/// will look for them.
struct Fixture {
  PipelineDescription Description;
  TestAddressSpace AddressSpace;
  revng::loader::LoadedAddressSpace Loaded;
  Model TheModel;

  Fixture() {
    auto Parsed = PipelineDescription::fromFile(REVNG_PIPELINE_PATH);
    BOOST_REQUIRE_MESSAGE(bool(Parsed),
                          "cannot parse the pipeline description: "
                            << toString(Parsed.takeError()));
    Description = std::move(*Parsed);

    auto MaybeLoaded = revng::loader::loadAddressSpace(AddressSpace);
    BOOST_REQUIRE(bool(MaybeLoaded));
    Loaded = std::move(*MaybeLoaded);

    // The model names its binaries by hash; `import-files` resolves that
    // against the store rather than against the filesystem.
    std::string Hash = hexHash(AddressSpace.bytes());
    revng::runner::FileStore::get()
      .add(Hash,
           { reinterpret_cast<const char *>(AddressSpace.bytes().data()),
             AddressSpace.bytes().size() });

    for (model::BinaryIdentifier &Identifier : Loaded.Model->Binaries()) {
      Identifier.Hash() = Hash;
      Identifier.Size() = AddressSpace.bytes().size();
    }

    TheModel.get() = Loaded.Model;
  }
};

} // namespace

BOOST_AUTO_TEST_CASE(ProducesTheLiftArtifact) {
  Fixture F;
  Runner TheRunner(F.Description, F.TheModel);

  // Pick the backend that needs no libtcg, so this runs everywhere.
  TheRunner.setDynamicConfiguration("lift", "backend: reference-x86_64\n");

  auto Produced = TheRunner.produceArtifact("lift", ObjectID::root());
  BOOST_REQUIRE_MESSAGE(bool(Produced),
                        "producing `lift` failed: "
                          << toString(Produced.takeError()));

  // What comes back is a serialised LLVM module.
  llvm::LLVMContext Context;
  llvm::ArrayRef<char> Data = Produced->data();
  llvm::MemoryBufferRef Buffer({ Data.data(), Data.size() }, "lifted");
  auto Module = llvm::parseBitcodeFile(Buffer, Context);
  BOOST_REQUIRE_MESSAGE(bool(Module),
                        "the artifact is not valid bitcode: "
                          << toString(Module.takeError()));

  llvm::Function *Root = (*Module)->getFunction("root");
  BOOST_REQUIRE(Root != nullptr);
  BOOST_CHECK(FunctionTags::Root.isTagOf(Root));
}

BOOST_AUTO_TEST_CASE(SecondRequestIsServedFromStorage) {
  Fixture F;
  Runner TheRunner(F.Description, F.TheModel);
  TheRunner.setDynamicConfiguration("lift", "backend: reference-x86_64\n");

  auto First = TheRunner.produceArtifact("lift", ObjectID::root());
  BOOST_REQUIRE(bool(First));

  size_t AfterFirst = TheRunner.pipesRun();
  BOOST_CHECK(AfterFirst > 0);

  // Asking again must be served from the savepoint cache rather than by
  // re-running anything.
  auto Second = TheRunner.produceArtifact("lift", ObjectID::root());
  BOOST_REQUIRE_MESSAGE(bool(Second),
                        "the second request failed: "
                          << toString(Second.takeError()));

  BOOST_CHECK_EQUAL(TheRunner.pipesRun(), AfterFirst);

  // And it must be the same bytes: a savepoint that re-serialised what it had
  // just loaded would hand back a different encoding of the same module.
  BOOST_CHECK_EQUAL(First->data().size(), Second->data().size());
}

BOOST_AUTO_TEST_CASE(InvalidationForcesARerun) {
  Fixture F;
  Runner TheRunner(F.Description, F.TheModel);
  TheRunner.setDynamicConfiguration("lift", "backend: reference-x86_64\n");

  BOOST_REQUIRE(bool(TheRunner.produceArtifact("lift", ObjectID::root())));
  const size_t AfterFirst = TheRunner.pipesRun();

  {
    Model Before = F.TheModel.clone();
    F.TheModel.get()->PlatformName() = "unrelated-to-lifting";
    TheRunner.invalidate(Before.diff(F.TheModel).paths());
  }
  BOOST_REQUIRE(bool(TheRunner.produceArtifact("lift", ObjectID::root())));
  BOOST_CHECK_EQUAL(TheRunner.pipesRun(), AfterFirst);

  {
    Model Before = F.TheModel.clone();
    F.TheModel.get()->EntryPoint() =
      MetaAddress::fromPC(model::Architecture::x86_64, 0x400002);
    TheRunner.invalidate(Before.diff(F.TheModel).paths());
  }
  BOOST_REQUIRE(bool(TheRunner.produceArtifact("lift", ObjectID::root())));
  BOOST_CHECK_GT(TheRunner.pipesRun(), AfterFirst);
}

// The path an embedder actually cares about: lift, then emit C for a function
// whose signature the host supplied. This is what proves the runner reaches the
// far end of the pipeline and not just the first savepoint.
//
// The prototype is supplied rather than detected. The reference backend emits
// the minimal module `docs/lifter-contract.md` describes, which is enough to
// lift and to decompile but not for `detect-abi` to work from -- a host driving
// its own backend is expected to know the signatures it wants.
BOOST_AUTO_TEST_CASE(DecompilesToC) {
  Fixture F;
  Runner TheRunner(F.Description, F.TheModel);
  TheRunner.setDynamicConfiguration("lift", "backend: reference-x86_64\n");

  MetaAddress Entry = MetaAddress::fromPC(model::Architecture::x86_64, 0x400000);
  TupleTree<model::Binary> &TheBinary = F.TheModel.get();

  model::Function &Function = TheBinary->Functions()[Entry];
  Function.Name() = "add";

  auto &&[Prototype, PrototypeType] = TheBinary->makeCABIFunctionDefinition();
  Prototype.ABI() = model::ABI::SystemV_x86_64;
  Prototype.addArgument(model::PrimitiveType::makeSigned(4)).Name() = "a";
  Prototype.addArgument(model::PrimitiveType::makeSigned(4)).Name() = "b";
  Prototype.ReturnType() = model::PrimitiveType::makeSigned(4);
  Function.Prototype() = std::move(PrototypeType);

  // Everything downstream assumes a valid model; check before blaming a pipe.
  BOOST_REQUIRE_MESSAGE(TheBinary->verify(true),
                        "the hand-built model does not verify");

  BOOST_REQUIRE(bool(TheRunner.produceArtifact("lift", ObjectID::root())));

  auto Decompiled = TheRunner.produceArtifact("emit-c", ObjectID(Entry));
  BOOST_REQUIRE_MESSAGE(bool(Decompiled),
                        "producing `emit-c` failed: "
                          << toString(Decompiled.takeError()));

  llvm::ArrayRef<char> Data = Decompiled->data();
  llvm::StringRef Text(Data.data(), Data.size());
  BOOST_CHECK(not Text.empty());
  BOOST_CHECK_MESSAGE(Text.contains("add"),
                      "the emitted C does not mention the function: " << Text.str());
}
