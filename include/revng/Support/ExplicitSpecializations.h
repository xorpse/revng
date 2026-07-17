#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#ifdef __cplusplus

#include <vector>

// Declare the existence of explicit template specializations of certain
// functions that would be otherwise heavy on build times. Make sure this file
// is included by a header that's included by all the translation units.

// This is a libc++ implementation detail that changed in recent Apple SDKs.
// It is only a build-time optimization, so keep it out of native Darwin builds.
#ifndef __APPLE__
extern template void std::vector<unsigned int>::__push_back_slow_path<
  const unsigned int &>(const unsigned int &);
#endif

#endif
