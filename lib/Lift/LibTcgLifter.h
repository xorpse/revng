#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Lift/AbstractLifter.h"

namespace revng::lift {

std::unique_ptr<ILifter>
createLibTcgLifter(const TupleTree<model::Binary> &Model);

} // namespace revng::lift
