#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/PipeboxCommon/Common.h"
#include "revng/PipeboxCommon/Model.h"

namespace revng::pypeline::analyses {

/// Give every unnamed field of a segment's type a name carrying its offset in
/// hexadecimal, which is how a disassembler shows the address.
///
/// Separate from `detect-segment-globals` because `analyze-data-layout` adds
/// fields of its own after it runs, and those need naming too. Only segments
/// are touched: an ordinary struct field's offset is not an address, so the
/// decimal name the model gives it stays.
class NameSegmentGlobals {
public:
  static constexpr llvm::StringRef Name = "name-segment-globals";

  llvm::Error
  run(Model &Model, const Request &Incoming, llvm::StringRef Configuration);
};

} // namespace revng::pypeline::analyses
