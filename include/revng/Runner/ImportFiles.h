#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/PipeboxCommon/BinariesContainer.h"
#include "revng/PipeboxCommon/Model.h"

namespace revng::pypeline::piperuns {

/// Populates the binaries container from the model's `Binaries`.
///
/// Upstream implements this pipe in Python, where it reaches the bytes through
/// a `FileProvider`. That is not reachable from a host that holds the bytes in
/// its own memory, so this resolves each identifier against
/// `revng::runner::FileStore` instead.
class ImportFiles {
private:
  const Model &TheModel;
  BinariesContainer &Output;

public:
  static constexpr llvm::StringRef Name = "import-files";
  using Arguments = TypeList<PipeRunArgument<BinariesContainer,
                                             "Output",
                                             "Binaries named by the model",
                                             Access::Write>>;

  ImportFiles(const Model &TheModel,
              llvm::StringRef StaticConfiguration,
              llvm::StringRef Configuration,
              BinariesContainer &Output) :
    TheModel(TheModel), Output(Output) {}

  void run();
};

} // namespace revng::pypeline::piperuns
