#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

# Backend-independent lifting support. Every translation unit belongs to
# exactly one library: several of the headers these use define `IRHelper`
# objects at namespace scope, and registering one twice is fatal, which is what
# compiling the same source into two dylibs would cause.
revng_add_library_internal(
  revngLift
  SHARED
  AbstractLifter.cpp
  CSVOffsets.cpp
  ExternalJumpsHandler.cpp
  IRAnnotators.cpp
  Lift.cpp
  LinkSupportPipe.cpp
  JumpTargetManager.cpp
  NoReturnAnalysis.cpp
  PostLiftVerifyPass.cpp
  RootAnalyzer.cpp
  SelfReferencingDbgAnnotationWriter.cpp
  ValueMaterializerPass.cpp)

target_link_libraries(
  revngLift
  PUBLIC ${CMAKE_DL_LIBS}
         m
         revngABI
         revngBasicAnalyses
         revngModelImporterDebugInfo
         revngFunctionCallIdentification
         revngModel
         revngSupport
         revngTypeShrinking
         revngEarlyFunctionAnalysis
         revngValueMaterializer
         ${LLVM_LIBRARIES})

# The libtcg backend. It needs a libtcg build, which is Linux-only, so it is
# optional; `revngLift` must not depend on it.
if(REVNG_BACKEND_LIBTCG)
  revng_add_library_internal(
    revngLiftLibTcg
    SHARED
    CodeGenerator.cpp
    InstructionTranslator.cpp
    LibTcg.cpp
    LibTcgLifter.cpp
    RegisterLibTcg.cpp
    VariableManager.cpp)

  target_link_libraries(revngLiftLibTcg PUBLIC revngLift ${LLVM_LIBRARIES})
endif()

# A minimal, helper-free backend. It is always built: it is the only backend
# available where libtcg is not, and it is what pins down the IR contract that
# an out-of-tree backend has to meet.
revng_add_library_internal(revngLiftReference SHARED ReferenceLifter.cpp)

target_link_libraries(revngLiftReference PUBLIC revngLift revngModel
                                                revngSupport ${LLVM_LIBRARIES})
