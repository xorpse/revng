#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

# `tests/unit` already links the examples against the build tree. What is left
# unproven is the installed prefix: the exported CMake package, the data files
# that land under `share/revng`, and whether the result can be moved. This test
# covers exactly that, so it installs into a scratch prefix of its own rather
# than reusing `CMAKE_INSTALL_PREFIX`.
add_test(
  NAME test_installed_sdk
  COMMAND
    "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/installed-sdk"
    "-DGENERATOR=${CMAKE_GENERATOR}" "-DBACKEND=reference-x86_64" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/run-installed-sdk-test.cmake")

# It links against what the build produced, so everything has to exist first.
set_tests_properties(
  test_installed_sdk
  PROPERTIES LABELS "integration" TIMEOUT 900 RUN_SERIAL TRUE FIXTURES_REQUIRED
             installed_sdk)
