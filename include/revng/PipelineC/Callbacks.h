#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <stdbool.h>
#include <stdint.h>

// Keep the C ABI header self-contained. This is the public opaque definition
// used by llvm-c/Types.h, without requiring consumers to install LLVM headers.
typedef struct LLVMOpaqueModule *LLVMModuleRef;

// Layout-compatible with MLIR's C API MlirModule without requiring MLIR
// headers in ordinary PipelineC consumers.
typedef struct rp_mlir_module {
  const void *ptr;
} rp_mlir_module;

#if defined(__cplusplus) && !defined(REVNG_PIPELINEC_C_ONLY)
class RawBinaryView;
typedef RawBinaryView rp_binary_view;
#else
typedef struct rp_binary_view rp_binary_view;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rp_address_space_mapping {
  const char *start;
  uint64_t virtual_size;
  uint64_t backing_size;
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
  bool (*mapping_at)(void *opaque, uint64_t index,
                     rp_address_space_mapping *mapping);
  bool (*read)(void *opaque, uint64_t mapping_index, uint64_t offset,
               uint8_t *destination, uint64_t size);
  uint64_t (*extra_code_address_count)(void *opaque);
  const char *(*extra_code_address_at)(void *opaque, uint64_t index);
  void (*release)(void *opaque);
} rp_address_space_callbacks;

typedef struct rp_file_address_space_mapping {
  const char *start;
  uint64_t virtual_size;
  uint64_t backing_size;
  const char *path;
  uint64_t file_offset;
  bool readable;
  bool writeable;
  bool executable;
  const char *name;
} rp_file_address_space_mapping;

typedef struct rp_lifter_callbacks {
  void *opaque;
  bool (*lift)(void *opaque, const void *model,
               const rp_binary_view *binary, const char *const entries[],
               uint64_t entry_count, LLVMModuleRef output,
               const char **error_message);
} rp_lifter_callbacks;

typedef struct rp_llvm_module_callbacks {
  void *opaque;
  bool (*transform)(void *opaque, LLVMModuleRef module,
                    const char **error_message);
} rp_llvm_module_callbacks;

typedef struct rp_mlir_module_callbacks {
  void *opaque;
  bool (*transform)(void *opaque, rp_mlir_module module,
                    const char **error_message);
} rp_mlir_module_callbacks;

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

typedef enum rp_type_kind {
  RP_TYPE_KIND_PRIMITIVE = 0,
  RP_TYPE_KIND_POINTER,
  RP_TYPE_KIND_ARRAY,
  RP_TYPE_KIND_DEFINED,
} rp_type_kind;

/**
 * A recursive type expression. For pointers and arrays, element_type points to
 * the pointee/element expression and size is the pointer size or element count
 * respectively. A pointer size of zero selects the model architecture's native
 * pointer size. Defined types refer to a definition returned by a create-type
 * API through definition_id.
 */
typedef struct rp_type {
  rp_type_kind kind;
  bool is_const;
  rp_primitive_type primitive;
  uint64_t size;
  uint64_t definition_id;
  const struct rp_type *element_type;
} rp_type;

typedef struct rp_typed_argument {
  const char *name;
  const char *comment;
  rp_type type;
} rp_typed_argument;

typedef struct rp_named_typed_register {
  const char *register_name;
  const char *name;
  const char *comment;
  rp_type type;
} rp_named_typed_register;

#ifdef __cplusplus
} // extern "C"
#endif
