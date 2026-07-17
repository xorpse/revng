//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Lift/AbstractLifter.h"

#include "LibTcgLifter.h"

namespace revng::lift {

namespace {
bool Registered = []() {
  llvm::cantFail(LifterRegistry::registerLifter("libtcg",
                                                createLibTcgLifter,
                                                true));
  return true;
}();
} // namespace

} // namespace revng::lift
