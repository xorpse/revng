#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

revng_add_library_internal(revngStorage SHARED StorageClient.cpp Path.cpp)

llvm_map_components_to_libnames(LLVM_LIBRARIES Core Support)
if(APPLE AND REVNG_STOCK_LLVM_COMPAT)
  set(LLVM_LIBRARIES LLVM)
endif()

target_link_libraries(revngStorage revngSupport ${LLVM_LIBRARIES})
