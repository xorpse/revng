#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <stdbool.h>
#include <stdint.h>

// Keep the C ABI header self-contained. This is the public opaque definition
// used by llvm-c/Types.h, without requiring consumers to install LLVM headers.
typedef struct LLVMOpaqueModule *LLVMModuleRef;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rp_address_space_mapping {
  const char *start;
  uint64_t virtual_size;
  const uint8_t *contents;
  uint64_t contents_size;
  bool readable;
  bool writeable;
  bool executable;
  const char *name;
} rp_address_space_mapping;

typedef struct rp_address_space_callbacks {
  void *opaque;
  const char *(*architecture)(void *opaque);
  const char *(*entry_point)(void *opaque);
  uint64_t (*mapping_count)(void *opaque);
  bool (*mapping_at)(void *opaque,
                     uint64_t index,
                     rp_address_space_mapping *mapping);
  uint64_t (*extra_code_address_count)(void *opaque);
  const char *(*extra_code_address_at)(void *opaque, uint64_t index);
} rp_address_space_callbacks;

typedef struct rp_lifter_callbacks {
  void *opaque;
  bool (*lift)(void *opaque,
               const char *model_yaml,
               const uint8_t *binary,
               uint64_t binary_size,
               const char *const entries[],
               uint64_t entry_count,
               LLVMModuleRef output,
               const char **error_message);
} rp_lifter_callbacks;

typedef enum rp_primitive_kind {
  RP_PRIMITIVE_KIND_VOID = 0,
  RP_PRIMITIVE_KIND_GENERIC,
  RP_PRIMITIVE_KIND_POINTER_OR_NUMBER,
  RP_PRIMITIVE_KIND_NUMBER,
  RP_PRIMITIVE_KIND_UNSIGNED,
  RP_PRIMITIVE_KIND_SIGNED,
  RP_PRIMITIVE_KIND_FLOAT,
} rp_primitive_kind;

typedef struct rp_primitive_type {
  rp_primitive_kind kind;
  uint64_t size;
} rp_primitive_type;

typedef struct rp_cabi_argument {
  const char *name;
  rp_primitive_type type;
} rp_cabi_argument;

#ifdef __cplusplus
} // extern "C"
#endif
