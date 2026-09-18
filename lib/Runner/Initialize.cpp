//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <mutex>

#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/TargetSelect.h"

#include "revng/Runner/Initialize.h"

namespace revng::runner {

void ensureLLVMInitialized() {
  static std::once_flag Flag;
  std::call_once(Flag, [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    llvm::PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();
    llvm::initializeCore(Registry);
    llvm::initializeTransformUtils(Registry);
    llvm::initializeScalarOpts(Registry);
    llvm::initializeVectorization(Registry);
    llvm::initializeInstCombine(Registry);
    llvm::initializeIPO(Registry);
    llvm::initializeAnalysis(Registry);
    llvm::initializeCodeGen(Registry);
    llvm::initializeGlobalISel(Registry);
    llvm::initializeTarget(Registry);
  });
}

} // namespace revng::runner
