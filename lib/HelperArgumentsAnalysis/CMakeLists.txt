#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

revng_add_analyses_library_internal(
  revngHelperArgumentsAnalysis
  AnalyzeHelperArguments.cpp
  Annotation.cpp
  ArgumentUsageAnalysis.cpp
  Context.cpp
  CPULoopExitPass.cpp
  CPUStateUsage.cpp
  HelpersModuleToDeclarations.cpp
  SlimDownHelpersModule.cpp
  Value.cpp)

if(REVNG_BACKEND_LIBTCG)
  target_sources(revngHelperArgumentsAnalysis PRIVATE FixHelpers.cpp)
endif()

target_link_libraries(revngHelperArgumentsAnalysis revngLift revngSupport
                      ${LLVM_LIBRARIES})
