#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

namespace revng::runner {

/// Populate LLVM's pass registry and native target.
///
/// Pipes that run LLVM passes look them up by name in the global pass registry,
/// which is empty until something fills it. `revng::InitRevng` does that for
/// revng's own tools, but it also parses the command line and installs signal
/// handlers, which a library embedded in someone else's process must not do.
/// This is the part an embedder does need, and it is idempotent.
void ensureLLVMInitialized();

} // namespace revng::runner
