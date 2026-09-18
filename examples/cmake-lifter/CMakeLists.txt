cmake_minimum_required(VERSION 3.20)
project(revng_c_consumer LANGUAGES C CXX)

find_package(revng REQUIRED CONFIG)

add_executable(revng_c_consumer ../lifter-library.c)
set_target_properties(revng_c_consumer PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(
  revng_c_consumer
  PRIVATE
    "REVNG_TEST_PIPELINE=\"${revng_DIR}/../pipelines/address-space.yml\"")
target_link_libraries(revng_c_consumer PRIVATE revngPipelineC
                                               revngLiftReference)

# Backends register from their shared-library initializers, so ensure the
# selected backend DSO remains a direct dependency of the executable.
if(APPLE)
  target_link_options(
    revng_c_consumer
    PRIVATE "-Wl,-needed_library,$<TARGET_FILE:revngLiftReference>")
else()
  target_link_options(revng_c_consumer PRIVATE -Wl,--no-as-needed)
endif()
