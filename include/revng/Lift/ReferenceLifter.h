#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Lift/AbstractLifter.h"

namespace revng::lift {

/// Create the small helper-free x86-64 backend used to exercise the public
/// backend contract. It intentionally supports only NOP (0x90) and RET (0xc3).
std::unique_ptr<ILifter> createReferenceX86Lifter();

} // namespace revng::lift
