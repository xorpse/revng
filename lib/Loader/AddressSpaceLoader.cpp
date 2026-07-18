//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <limits>

#include "revng/Loader/AddressSpaceLoader.h"
#include "revng/Model/ABI.h"
#include "revng/Model/BinaryIdentifier.h"
#include "revng/Model/Segment.h"
#include "revng/Support/Error.h"

namespace revng::loader {

static llvm::Error validateAddress(const MetaAddress &Address,
                                   model::Architecture::Values Architecture,
                                   llvm::StringRef Description) {
  if (Address.isInvalid())
    return revng::createError(Description + " is invalid");
  const unsigned
    PointerSize = model::Architecture::getPointerSize(Architecture);
  const bool CompatibleGeneric = Address.isGeneric()
                                 and Address.bitSize() == PointerSize * 8;
  if (not CompatibleGeneric and Address.arch() != Architecture)
    return revng::createError(Description + " has an unexpected architecture");
  return llvm::Error::success();
}

llvm::Expected<LoadedAddressSpace>
loadAddressSpace(const AbstractAddressSpace &AddressSpace,
                 bool MaterializeData) {
  const auto Architecture = AddressSpace.architecture();
  if (Architecture == model::Architecture::Invalid)
    return revng::createError("address space has an invalid architecture");

  LoadedAddressSpace Result;
  Result.Model->Architecture() = Architecture;
  if (auto ABI = model::ABI::getDefaultForELF(Architecture))
    Result.Model->DefaultABI() = *ABI;

  model::BinaryIdentifier Identifier;
  Identifier.Index() = 0;
  Identifier.Hash() = std::string(64, '0');
  Identifier.CanonicalPath() = "<abstract-address-space>";
  Result.Model->Binaries().insert(std::move(Identifier));
  auto BinaryReference = Result.Model->getBinaryIdentifierReference(0);

  if (auto Entry = AddressSpace.entryPoint()) {
    if (auto Error = validateAddress(*Entry, Architecture, "entry point"))
      return std::move(Error);
    Result.Model->EntryPoint() = *Entry;
  }

  uint64_t NextOffset = 0;
  for (const Mapping &Mapping : AddressSpace.mappings()) {
    if (auto Error = validateAddress(Mapping.Start,
                                     Architecture,
                                     "mapping start"))
      return std::move(Error);
    if (Mapping.VirtualSize == 0)
      return revng::createError("address-space mappings cannot be empty");
    const uint64_t BackingSize = Mapping.BackingSize == 0 ?
                                   Mapping.Contents.size() :
                                   Mapping.BackingSize;
    if (Mapping.Contents.size() > BackingSize)
      return revng::createError("mapping contents exceed its backing size");
    if (BackingSize > Mapping.VirtualSize)
      return revng::createError("mapping contents exceed its virtual size");
    if (Mapping.VirtualSize > std::numeric_limits<uint64_t>::max() - NextOffset)
      return revng::createError("flattened address-space buffer is too large");

    const uint64_t StartOffset = NextOffset;
    NextOffset += Mapping.VirtualSize;
    model::Segment Segment({ Mapping.Start, Mapping.VirtualSize });
    Segment.Binary() = BinaryReference;
    Segment.StartOffset() = StartOffset;
    Segment.FileSize() = BackingSize;
    Segment.IsReadable() = Mapping.Readable;
    Segment.IsWriteable() = Mapping.Writeable;
    Segment.IsExecutable() = Mapping.Executable;

    if (not Result.Model->Segments().insert(std::move(Segment)).second)
      return revng::createError("duplicate address-space mapping");

    if (MaterializeData) {
      if (NextOffset > Result.Data.max_size())
        return revng::createError("flattened address-space buffer is too large");
      Result.Data.resize(NextOffset, 0);
      std::copy(Mapping.Contents.begin(),
                Mapping.Contents.end(),
                Result.Data.begin() + StartOffset);
    }
  }

  for (MetaAddress Address : AddressSpace.extraCodeAddresses()) {
    if (auto Error = validateAddress(Address,
                                     Architecture,
                                     "extra code address"))
      return std::move(Error);
    Result.Model->ExtraCodeAddresses().insert(Address);
  }

  if (auto Error = RawBinaryView::checkPrecondition(*Result.Model))
    return std::move(Error);
  return Result;
}

} // namespace revng::loader
