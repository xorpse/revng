#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

revng_add_library_internal(
  revngLift
  SHARED
  AbstractLifter.cpp
  CSVOffsets.cpp
  IRAnnotators.cpp
  Lift.cpp
  LiftPipe.cpp
  LinkSupportPipe.cpp
  LoadBinaryPass.cpp
  PostLiftVerifyPass.cpp
  SelfReferencingDbgAnnotationWriter.cpp)

target_link_libraries(
  revngLift
  PUBLIC m
         revngABI
         revngBasicAnalyses
         revngModel
         revngPipes
         revngSupport
         ${LLVM_LIBRARIES})

if(REVNG_BACKEND_LIBTCG)
  revng_add_library_internal(
    revngLiftLibTcg
    SHARED
    CodeGenerator.cpp
    ExternalJumpsHandler.cpp
    InstructionTranslator.cpp
    JumpTargetManager.cpp
    LibTcg.cpp
    LibTcgLifter.cpp
    RootAnalyzer.cpp
    ValueMaterializerPass.cpp
    VariableManager.cpp)

  target_link_libraries(
    revngLiftLibTcg
    PUBLIC ${CMAKE_DL_LIBS}
           m
           revngABI
           revngBasicAnalyses
           revngModelImporterDebugInfo
           revngFunctionCallIdentification
           revngModel
           revngPipes
           revngSupport
           revngTypeShrinking
           revngEarlyFunctionAnalysis
           revngValueMaterializer
           ${LLVM_LIBRARIES})

  target_sources(revngLift PRIVATE RegisterLibTcg.cpp)
  target_link_libraries(revngLift PUBLIC revngLiftLibTcg)
endif()

revng_add_library_internal(revngLiftReference SHARED ReferenceLifter.cpp)
target_link_libraries(revngLiftReference PUBLIC revngLift revngModel
                                                revngSupport ${LLVM_LIBRARIES})
