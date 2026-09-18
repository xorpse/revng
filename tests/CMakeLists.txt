#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

# The unit tests exercise the libraries directly, so they are the only ones an
# SDK build can run: the others drive `revng` itself or the Python bindings,
# neither of which that configuration produces.
add_subdirectory(unit)

# Builds an example against the *installed* prefix, so it is the only test that
# exercises the exported CMake package and the relocatability of the tree.
add_subdirectory(library-sdk)

if(NOT REVNG_SDK_BUILD)
  add_subdirectory(abi)
  add_subdirectory(filecheck)
  add_subdirectory(pypeline)
endif()
