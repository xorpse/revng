#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

# An in-process executor for the pipeline, for embedders that cannot use the
# Python orchestrator: it dispatches each task to a short-lived worker process
# that exchanges containers as files, which host callbacks cannot survive.
revng_add_library_internal(revngRunner SHARED PipelineDescription.cpp
                           ImportFiles.cpp Requests.cpp
                           MemoryStorage.cpp Runner.cpp
                           Initialize.cpp)

target_link_libraries(revngRunner PUBLIC revngModel revngSupport
                                         ${LLVM_LIBRARIES})

if(REVNG_PIPEBOX_PYTHON)
  target_compile_definitions(revngRunner PRIVATE REVNG_PIPEBOX_PYTHON)
  target_link_libraries(revngRunner PRIVATE ${Python3_LIBRARIES} nanobind)
  target_include_directories(revngRunner PRIVATE ${Python3_INCLUDE_DIRS})
endif()
