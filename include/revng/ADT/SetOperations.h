#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/SetOperations.h"

template<class S1Ty, class S2Ty>
S1Ty setIntersection(const S1Ty &S1, const S2Ty &S2) {
  S1Ty Result;
  for (const auto &Element : S1)
    if (S2.count(Element))
      Result.insert(Element);
  return Result;
}

template<class S1Ty, class S2Ty>
bool intersects(const S1Ty &S1, const S2Ty &S2) {
  S1Ty Intersection = setIntersection(S1, S2);
  return !Intersection.empty();
}

template<class S1Ty, class S2Ty>
bool equal(const S1Ty &S1, const S2Ty &S2) {
  return llvm::set_is_subset(S1, S2) and llvm::set_is_subset(S2, S1);
}
