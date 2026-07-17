#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

revng_add_library_internal(
  revngPipes
  SHARED
  Containers.cpp
  IRHelpers.cpp
  PipelineManager.cpp
  Pipes.cpp
  RootKind.cpp
  FunctionPass.cpp
  TaggedFunctionKind.cpp
  GlobalsAnalyses.cpp
  DebugInfoHelpers.cpp)

llvm_map_components_to_libnames(LLVM_LIBRARIES Core MC Support)
if(APPLE AND REVNG_STOCK_LLVM_COMPAT)
  set(LLVM_LIBRARIES LLVM)
endif()
target_link_libraries(revngPipes revngPipeline revngModel revngSupport
                      revngStorage ${LLVM_LIBRARIES})

if(TARGET revngFunctionCallIdentification)
  target_link_libraries(revngPipes revngFunctionCallIdentification)
  if(APPLE)
    target_link_options(
      revngPipes
      PRIVATE
        "-Wl,-needed_library,$<TARGET_FILE:revngFunctionCallIdentification>")
  else()
    target_link_options(revngPipes PRIVATE -Wl,--no-as-needed)
  endif()
endif()
