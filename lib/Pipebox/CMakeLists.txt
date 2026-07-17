#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

revng_add_library_internal(revngPipebox SHARED Pipebox.cpp LLVMPipe.cpp
                           MLIRPipe.cpp MergeLLVMModules.cpp)

llvm_map_components_to_libnames(LLVM_LIBRARIES Support Core)
target_link_libraries(
  revngPipebox
  PUBLIC
  ${Python3_LIBRARIES}
  ${LLVM_LIBRARIES}
  revngCanonicalize
  revngClift
  revngCliftPipes
  revngDataLayoutAnalysis
  revngFunctionCallIdentification
  revngFunctionIsolation
  revngImportFromCAnalysis
  revngLLMRenameAnalysis
  revngLift
  revngModelImporter
  revngPromoteStackPointer
  revngRecompile
  revngRemoveExtractValues
  revngRemoveLiftingArtifacts
  revngSegmentReferences
  revngYield
  revngYieldPipes
  revngSupport
  PRIVATE
  $<BUILD_INTERFACE:nanobind>)

target_include_directories(revngPipebox PUBLIC ${Python3_INCLUDE_DIRS})
target_compile_features(revngPipebox PRIVATE cxx_std_20)

# This library contributes LLVM passes through static registration and does
# not otherwise expose a symbol used by Pipebox. Retain the DSO even when
# libtcg (which historically pulled it in transitively) is disabled.
if(APPLE)
  target_link_options(
    revngPipebox
    PRIVATE
      "-Wl,-needed_library,$<TARGET_FILE:revngFunctionCallIdentification>")
else()
  target_link_options(revngPipebox PRIVATE -Wl,--no-as-needed)
endif()
