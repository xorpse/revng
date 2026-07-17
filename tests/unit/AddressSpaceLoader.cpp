//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE AddressSpaceLoader
#include <array>

#include "boost/test/unit_test.hpp"

#include "revng/Loader/AddressSpaceLoader.h"
#include "revng/UnitTestHelpers/UnitTestHelpers.h"

using namespace revng::loader;

namespace {

class TestAddressSpace final : public AbstractAddressSpace {
public:
  std::array<uint8_t, 3> Code = { 0x11, 0x22, 0x33 };
  std::array<uint8_t, 2> Data = { 0x44, 0x55 };
  std::array<Mapping, 2> Mappings;
  std::array<MetaAddress, 1> Extra;

  TestAddressSpace() :
    Mappings({ Mapping{
                 MetaAddress::fromPC(model::Architecture::x86_64, 0x1000),
                 5,
                 Code,
                 true,
                 false,
                 true,
                 "code" },
               Mapping{
                 MetaAddress::fromGeneric(model::Architecture::x86_64, 0x2000),
                 2,
                 Data,
                 true,
                 true,
                 false,
                 "data" } }),
    Extra({ MetaAddress::fromPC(model::Architecture::x86_64, 0x1002) }) {}

  model::Architecture::Values architecture() const override {
    return model::Architecture::x86_64;
  }
  std::optional<MetaAddress> entryPoint() const override {
    return MetaAddress::fromPC(model::Architecture::x86_64, 0x1000);
  }
  llvm::ArrayRef<Mapping> mappings() const override { return Mappings; }
  llvm::ArrayRef<MetaAddress> extraCodeAddresses() const override {
    return Extra;
  }
};

} // namespace

BOOST_AUTO_TEST_CASE(FlattenAndBuildModel) {
  TestAddressSpace AddressSpace;
  auto LoadedOrError = loadAddressSpace(AddressSpace);
  BOOST_REQUIRE(bool(LoadedOrError));
  LoadedAddressSpace Loaded = std::move(*LoadedOrError);

  BOOST_TEST(Loaded.Model->Architecture() == model::Architecture::x86_64);
  BOOST_CHECK(Loaded.Model->EntryPoint() == AddressSpace.entryPoint().value());
  BOOST_TEST(Loaded.Model->Binaries().size() == 1U);
  BOOST_TEST(Loaded.Model->Segments().size() == 2U);
  BOOST_TEST(Loaded.Model->ExtraCodeAddresses()
               .contains(AddressSpace.Extra[0]));

  const model::Segment *CodeSegmentPointer = nullptr;
  const model::Segment *DataSegmentPointer = nullptr;
  for (const model::Segment &Segment : Loaded.Model->Segments()) {
    if (Segment.IsExecutable())
      CodeSegmentPointer = &Segment;
    else
      DataSegmentPointer = &Segment;
  }
  BOOST_REQUIRE(CodeSegmentPointer != nullptr);
  BOOST_REQUIRE(DataSegmentPointer != nullptr);
  const model::Segment &CodeSegment = *CodeSegmentPointer;
  const model::Segment &DataSegment = *DataSegmentPointer;
  BOOST_TEST(CodeSegment.StartOffset() == 0U);
  BOOST_TEST(CodeSegment.FileSize() == 3U);
  BOOST_TEST(CodeSegment.VirtualSize() == 5U);
  BOOST_TEST(CodeSegment.IsExecutable());
  BOOST_TEST(not CodeSegment.IsWriteable());
  BOOST_TEST(DataSegment.StartOffset() == 5U);
  BOOST_TEST(DataSegment.FileSize() == 2U);
  BOOST_TEST(DataSegment.IsWriteable());

  RawBinaryView View = Loaded.view();
  auto Code = View.getByAddress(AddressSpace.Mappings[0].Start, 3);
  BOOST_REQUIRE(Code.has_value());
  BOOST_TEST((*Code)[0] == 0x11U);
  BOOST_TEST((*Code)[2] == 0x33U);

  auto ZeroFill = View.getByAddress(AddressSpace.Mappings[0].Start + 3, 2);
  BOOST_REQUIRE(ZeroFill.has_value());
  BOOST_TEST((*ZeroFill)[0] == 0U);
  BOOST_TEST((*ZeroFill)[1] == 0U);

  auto Data = View.getByAddress(AddressSpace.Mappings[1].Start, 2);
  BOOST_REQUIRE(Data.has_value());
  BOOST_TEST((*Data)[0] == 0x44U);
  BOOST_TEST((*Data)[1] == 0x55U);
}

BOOST_AUTO_TEST_CASE(RejectInvalidMappings) {
  TestAddressSpace AddressSpace;
  AddressSpace.Mappings[0].VirtualSize = 2;
  auto Loaded = loadAddressSpace(AddressSpace);
  BOOST_TEST(not bool(Loaded));
  llvm::consumeError(Loaded.takeError());
}
