#pragma once

#ifndef REVNG_STOCK_LLVM_COMPAT
#include_next "llvm/Support/Progress.h"
#else

// Compatibility implementation for stock LLVM. revng's LLVM fork provides a
// progress-reporting API that is not part of released LLVM. The task API is
// observational only, so native macOS library builds can safely use no-op
// tasks while retaining identical call sites.

#include <cstddef>
#include <optional>

#include "llvm/ADT/Twine.h"

namespace llvm {

class Task {
public:
  Task(std::optional<std::size_t>, const Twine &) {}

  void advance(const Twine &, bool = false) {}
};

} // namespace llvm

#endif
