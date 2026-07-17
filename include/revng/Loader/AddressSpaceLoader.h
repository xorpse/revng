#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>
#include <vector>

#include "llvm/Support/Error.h"

#include "revng/Loader/AbstractAddressSpace.h"
#include "revng/Model/Binary.h"
#include "revng/Model/RawBinaryView.h"
#include "revng/TupleTree/TupleTree.h"

namespace revng::loader {

/// The model and flat backing buffer consumed by the existing lift pipeline.
/// The RawBinaryView returned by view() remains valid while this object lives.
struct LoadedAddressSpace {
  TupleTree<model::Binary> Model;
  std::vector<uint8_t> Data;

  RawBinaryView view() const { return RawBinaryView(*Model, Data); }
};

/// Adapt a host address space to rev.ng's model plus flat-buffer
/// representation.
llvm::Expected<LoadedAddressSpace>
loadAddressSpace(const AbstractAddressSpace &AddressSpace);

} // namespace revng::loader
