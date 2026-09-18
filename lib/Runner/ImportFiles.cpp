//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/Support/raw_ostream.h"

#include "revng/PipeboxCommon/Helpers/Registrars.h"
#include "revng/Runner/FileStore.h"
#include "revng/Runner/ImportFiles.h"
#include "revng/Support/Assert.h"
#include "revng/Support/Tar.h"

namespace revng::runner {

FileStore &FileStore::get() {
  // Deliberately out of line: a function-local static in a header would give
  // each loaded image its own copy, and the embedder and the pipe are not
  // necessarily in the same one.
  static FileStore Instance;
  return Instance;
}

void FileStore::add(llvm::StringRef Hash, llvm::ArrayRef<char> Data) {
  std::lock_guard Guard(Mutex);
  Files[Hash] = std::vector<char>(Data.begin(), Data.end());
}

std::optional<llvm::ArrayRef<char>> FileStore::find(llvm::StringRef Hash) const {
  std::lock_guard Guard(Mutex);
  auto It = Files.find(Hash);
  if (It == Files.end())
    return std::nullopt;
  return llvm::ArrayRef<char>(It->second);
}

void FileStore::erase(llvm::StringRef Hash) {
  std::lock_guard Guard(Mutex);
  Files.erase(Hash);
}

} // namespace revng::runner

namespace revng::pypeline::piperuns {

void ImportFiles::run() {
  const model::Binary &Binary = *TheModel.get().get();

  // `BinariesContainer` only takes input through `deserialize`, whose format is
  // a plain tar whose members are named `binaries/<hash>`.
  revng::pypeline::Buffer Archive;
  {
    llvm::raw_svector_ostream Stream(Archive.data());
    TarWriter Writer(Stream, TarFormat::Plain);

    for (const model::BinaryIdentifier &Identifier : Binary.Binaries()) {
      std::optional Data = revng::runner::FileStore::get().find(Identifier
                                                                 .Hash());
      revng_assert(Data.has_value(),
                   ("no bytes were provided for binary " + Identifier.Hash())
                     .c_str());
      Writer.addMember("binaries/" + Identifier.Hash(),
                       { Data->data(), Data->size() });
    }
  }

  ObjectID Root;
  std::map<const ObjectID *, llvm::ArrayRef<char>> Input;
  llvm::ArrayRef<char> Contents = { Archive.data().data(),
                                    Archive.data().size() };
  Input.emplace(&Root, Contents);
  Output.deserialize(Input);
}

} // namespace revng::pypeline::piperuns

// Registered from this library rather than from `lib/Pipebox`, which holds the
// pipes upstream implements in C++. The Python side has its own `import-files`
// and the two registries are separate, so they do not collide.
static RegisterSingleOutputPipeRun<revng::pypeline::piperuns::ImportFiles>
  RegisterImportFiles;
