#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

include(CMakeFindDependencyMacro)
find_dependency(LLVM REQUIRED CONFIG)
find_dependency(LibArchive REQUIRED)

get_filename_component(revng_CMAKE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
include("${revng_CMAKE_DIR}/revng.cmake")
set_property(TARGET revngPipelineC APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                                                   "${LLVM_INCLUDE_DIRS}")
include("${revng_CMAKE_DIR}/Common.cmake")
