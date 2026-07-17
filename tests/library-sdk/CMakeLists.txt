#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

# Compile the example as C, while using the C++ linker driver for revng's C++
# runtime dependencies. The reference backend registers itself at load time.
add_executable(test_lifter_library_c
               "${CMAKE_SOURCE_DIR}/examples/lifter-library.c")
set_target_properties(test_lifter_library_c PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(
  test_lifter_library_c
  PRIVATE
    "REVNG_TEST_PIPELINE=\"${CMAKE_SOURCE_DIR}/tests/unit/PipelineCAddressSpace.yml\"")
if(APPLE)
  target_link_options(
    test_lifter_library_c
    PRIVATE "-Wl,-needed_library,$<TARGET_FILE:revngLiftReference>")
else()
  target_link_options(test_lifter_library_c PRIVATE -Wl,--no-as-needed)
endif()
target_link_libraries(test_lifter_library_c revngPipelineC revngLiftReference)

add_test(NAME test_lifter_library_c_reference
         COMMAND test_lifter_library_c reference-x86_64)
set_tests_properties(test_lifter_library_c_reference PROPERTIES LABELS
                                                                "integration")
if(REVNG_BACKEND_LIBTCG)
  add_test(NAME test_lifter_library_c_libtcg
           COMMAND test_lifter_library_c libtcg)
  set_tests_properties(test_lifter_library_c_libtcg PROPERTIES LABELS
                                                               "integration")
endif()
