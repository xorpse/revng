#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>

#include "revng/Model/GlobalVariableBuilder.h"
#include "revng/PipeboxCommon/Common.h"
#include "revng/PipeboxCommon/LLVMContainer.h"
#include "revng/PipeboxCommon/Model.h"
#include "revng/SegmentReferences/SegmentUsesEnumerator.h"

/// Analyse segment references in the code, and add a field to the struct
/// describing the segment for each address the code loads from or stores to.
///
/// This is what gives a bare `segment_N + offset` a name: without a field
/// covering the address there is nothing for the decompiler to call it.
/// `DetectCStrings` does the same for string constants, and runs first so that
/// a string wins the hole over a plain scalar.
class DetectSegmentGlobals {
private:
  const model::Binary &Binary;
  SegmentUsesEnumerator SegmentUses;
  model::GlobalVariableBuilder GlobalBuilder;

  /// The widest access seen at each address, in bytes. Ordered, so that what
  /// `commit` inserts does not depend on the order the uses came in.
  std::map<MetaAddress, uint64_t> Widths;

public:
  DetectSegmentGlobals(model::Binary &Binary) :
    Binary(Binary),
    SegmentUses(Binary, SegmentUsesEnumerator::SegmentAccess::All),
    GlobalBuilder(Binary) {}

public:
  void run(llvm::Module &M, llvm::Function *LimitTo = nullptr);

  void commit();
};

namespace revng::pypeline::analyses {

class DetectSegmentGlobals {
public:
  static constexpr llvm::StringRef Name = "detect-segment-globals";

  llvm::Error run(Model &Model,
                  const Request &Incoming,
                  llvm::StringRef Configuration,
                  LLVMFunctionContainer &ModuleContainer);
};

} // namespace revng::pypeline::analyses
