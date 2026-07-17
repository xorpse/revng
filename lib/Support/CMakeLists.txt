#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

if(REVNG_STOCK_LLVM_COMPAT)
  set(REVNG_PROGRESS_SOURCE)
else()
  set(REVNG_PROGRESS_SOURCE Progress.cpp)
endif()

revng_add_library_internal(
  revngSupport
  SHARED
  Configuration.cpp
  ProgramRunner.cpp
  Assert.cpp
  BasicBlockID.cpp
  CDataModel.cpp
  CommandLine.cpp
  CommonOptions.cpp
  CustomizedLLVMPasses.cpp
  Debug.cpp
  DropRedundantPtrIntCasts.cpp
  ExplicitSpecializations.cpp
  FileSystem.cpp
  InitRevng.cpp
  IRHelpers.cpp
  MetaAddress.cpp
  ModuleStatistics.cpp
  OnQuit.cpp
  PathList.cpp
  ${REVNG_PROGRESS_SOURCE}
  ResourceFinder.cpp
  Statistics.cpp
  Tag.cpp
  GzipTarFile.cpp
  Tar.cpp
  GzipStream.cpp
  WindowsApiSetSchemaParser.cpp
  Unicode.cpp
  ZstdStream.cpp)

include(FindLibArchive)

if(NOT LibArchive_FOUND)
  message(FATAL_ERROR "libarchive not found")
endif()

llvm_map_components_to_libnames(
  LLVM_LIBRARIES
  Analysis
  BitReader
  BitWriter
  CodeGen
  Core
  GlobalISel
  InstCombine
  IPO
  IRReader
  Linker
  Object
  Passes
  ScalarOpts
  Support
  Target
  TransformUtils
  Vectorize
  X86AsmParser
  X86CodeGen
  X86Desc
  X86Info)

if(APPLE AND NOT REVNG_STOCK_LLVM_COMPAT)
  llvm_map_components_to_libnames(
    LLVM_NATIVE_LIBRARIES
    AArch64AsmParser
    AArch64CodeGen
    AArch64Desc
    AArch64Info)
endif()

if(APPLE AND REVNG_STOCK_LLVM_COMPAT)
  set(LLVM_LIBRARIES LLVM)
  set(LLVM_NATIVE_LIBRARIES)
endif()

target_link_libraries(revngSupport z zstd LibArchive::LibArchive
                      ${LLVM_LIBRARIES} ${LLVM_NATIVE_LIBRARIES})

target_include_directories(revngSupport INTERFACE $<INSTALL_INTERFACE:include/>)

# revngSupport has a dependency to the generated model files because of revng.h
#
# revng.h includes model headers which depends on the generated headers. we
# cannot add the dependency to the model because the model itself depends on
# support and would generate a cyclic dependency.
#
# TODO: drop dependency from model in revng.h
add_dependencies(revngSupport generate-revngModel-tuple-tree-code)
