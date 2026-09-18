#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

revng_add_library_internal(revngLoader SHARED AddressSpaceLoader.cpp)

target_link_libraries(revngLoader PUBLIC revngModel revngSupport
                                         ${LLVM_LIBRARIES})
