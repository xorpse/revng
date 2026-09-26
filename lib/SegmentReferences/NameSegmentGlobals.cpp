//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"

#include "revng/Model/Binary.h"
#include "revng/SegmentReferences/NameSegmentGlobals.h"
#include "revng/Support/Debug.h"

static Logger Log("name-segment-globals");

namespace revng::pypeline::analyses {

llvm::Error NameSegmentGlobals::run(Model &Model,
                                    const Request &Incoming,
                                    llvm::StringRef Configuration) {
  model::Binary &Binary = *Model.get().get();

  for (model::Segment &Segment : Binary.Segments()) {
    model::StructDefinition *Struct = Segment.type();
    if (Struct == nullptr)
      continue;

    // Collected first: assigning through the iterator mutates the container
    // being walked, and only the first field ends up named.
    llvm::SmallVector<uint64_t, 16> Offsets;
    for (const model::StructField &Field : Struct->Fields())
      if (Field.Name().empty())
        Offsets.push_back(Field.Offset());

    for (uint64_t Offset : Offsets) {
      // `offset_<decimal>` is reserved for the name the model makes up when a
      // field has none, so a bare hex offset that happens to be all digits is
      // rejected. The `0x` both avoids that and says which base this is.
      Struct->Fields().at(Offset).Name() = "offset_0x"
                                           + llvm::utohexstr(Offset, true, 0);
      revng_log(Log,
                "Named the field at " << Offset << " of "
                                      << Segment.StartAddress().toString());
    }
  }

  return llvm::Error::success();
}

} // namespace revng::pypeline::analyses
