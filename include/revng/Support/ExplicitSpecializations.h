#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#ifdef __cplusplus

#include <vector>

// Declare the existence of explicit template specializations of certain
// functions that would be otherwise heavy on build times. Make sure this file
// is included by a header that's included by all the translation units.

// __push_back_slow_path is a libc++ internal whose signature returned void
// until libc++ 19 (it now returns a pointer). It is only a build-time
// optimization, so restrict it to libc++ versions that still match.
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 190000
extern template void std::vector<unsigned int>::__push_back_slow_path<
  const unsigned int &>(const unsigned int &);
#endif

#endif
