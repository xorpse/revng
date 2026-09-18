#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

# Build `examples/cmake-lifter` the way a downstream project would: against an
# installed prefix through `find_package(revng)`, with no access to the build
# tree's cache. Everything the consumer needs therefore has to reach it through
# the exported configuration, which is the whole point of the exercise -- an
# in-tree test inherits the cache and proves nothing about the export.

foreach(REQUIRED BUILD_DIR SOURCE_DIR WORK_DIR GENERATOR BACKEND)
  if(NOT ${REQUIRED})
    message(FATAL_ERROR "run-installed-sdk-test.cmake: -D${REQUIRED} is required")
  endif()
endforeach()

set(PREFIX "${WORK_DIR}/prefix")
set(CONSUMER_BUILD "${WORK_DIR}/consumer")

function(run_step DESCRIPTION)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE STATUS
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE OUTPUT)
  if(NOT STATUS EQUAL 0)
    message(FATAL_ERROR "${DESCRIPTION} failed (${STATUS}):\n${OUTPUT}")
  endif()
  set(LAST_OUTPUT "${OUTPUT}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

run_step("install" "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}")

# Deliberately pass only `revng_DIR`. If the consumer needs an LLVM or
# libarchive location, the exported configuration has to supply it.
run_step("consumer configure" "${CMAKE_COMMAND}" -S
         "${SOURCE_DIR}/examples/cmake-lifter" -B "${CONSUMER_BUILD}" -G
         "${GENERATOR}" "-Drevng_DIR=${PREFIX}/share/revng/cmake")

run_step("consumer build" "${CMAKE_COMMAND}" --build "${CONSUMER_BUILD}")

run_step("consumer run" "${CONSUMER_BUILD}/revng_c_consumer" "${BACKEND}")
if(NOT LAST_OUTPUT MATCHES "produced [1-9][0-9]* bytes")
  message(FATAL_ERROR "consumer produced no module:\n${LAST_OUTPUT}")
endif()
message(STATUS "consumer output: ${LAST_OUTPUT}")

# Relocatability: nothing in the install tree may name the build or source tree
# it came from, or the prefix stops being movable.
file(GLOB_RECURSE INSTALLED_BINARIES "${PREFIX}/lib/*.dylib" "${PREFIX}/lib/*.so"
     "${PREFIX}/lib/*.so.*")
if(NOT INSTALLED_BINARIES)
  message(FATAL_ERROR "no shared libraries found under ${PREFIX}/lib")
endif()

find_program(OTOOL otool)
foreach(BINARY ${INSTALLED_BINARIES})
  if(OTOOL)
    execute_process(COMMAND "${OTOOL}" -l "${BINARY}" OUTPUT_VARIABLE LOAD_COMMANDS)
  else()
    execute_process(COMMAND objdump -p "${BINARY}" OUTPUT_VARIABLE LOAD_COMMANDS)
  endif()
  # Both tools echo the path they were handed, and this prefix sits inside the
  # build tree, so that line would match every pattern below on its own.
  string(REPLACE "${BINARY}" "" LOAD_COMMANDS "${LOAD_COMMANDS}")
  foreach(FORBIDDEN "${BUILD_DIR}" "${SOURCE_DIR}/lib" "${SOURCE_DIR}/tools")
    if(LOAD_COMMANDS MATCHES "${FORBIDDEN}")
      get_filename_component(NAME_ONLY "${BINARY}" NAME)
      message(FATAL_ERROR
              "${NAME_ONLY} still refers to ${FORBIDDEN}; the prefix is not relocatable")
    endif()
  endforeach()
endforeach()
list(LENGTH INSTALLED_BINARIES BINARY_COUNT)
message(STATUS "${BINARY_COUNT} installed libraries carry no build-tree paths")
