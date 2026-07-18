//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "revng/PipelineC/PipelineC.h"

struct address_space {
  const uint8_t *bytes;
  uint64_t size;
};

static const char *architecture(void *opaque) {
  (void) opaque;
  return "x86_64";
}
static const char *entry_point(void *opaque) {
  (void) opaque;
  return "0x400000:Code_x86_64";
}
static uint64_t mapping_count(void *opaque) {
  (void) opaque;
  return 1;
}
static bool mapping_at(void *opaque,
                       uint64_t index,
                       rp_address_space_mapping *output) {
  struct address_space *space = opaque;
  if (index != 0)
    return false;
  *output = (rp_address_space_mapping){
    .start = "0x400000:Code_x86_64",
    .virtual_size = space->size,
    .backing_size = space->size,
    .readable = true,
    .writeable = false,
    .executable = true,
    .name = "add-code",
  };
  return true;
}
static bool read_bytes(void *opaque,
                       uint64_t mapping_index,
                       uint64_t offset,
                       uint8_t *destination,
                       uint64_t size) {
  struct address_space *space = opaque;
  if (mapping_index != 0 || offset > space->size
      || size > space->size - offset)
    return false;
  memcpy(destination, space->bytes + offset, size);
  return true;
}

static bool contains(const char *data, uint64_t size, const char *needle) {
  const size_t needle_size = strlen(needle);
  if (needle_size > size)
    return false;
  for (uint64_t i = 0; i <= size - needle_size; ++i)
    if (memcmp(data + i, needle, needle_size) == 0)
      return true;
  return false;
}

static void print_error(const char *operation, rp_error *error) {
  rp_simple_error *simple = rp_error_get_simple_error(error);
  const char *message = simple == NULL ? NULL :
                                         rp_simple_error_get_message(simple);
  fprintf(stderr,
          "%s failed%s%s\n",
          operation,
          message == NULL ? "" : ": ",
          message == NULL ? "" : message);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s {reference-x86_64|libtcg}\n", argv[0]);
    return 2;
  }
  char pipeline_option[4096];
  int written = snprintf(pipeline_option,
                         sizeof(pipeline_option),
                         "--pipeline-path=%s",
                         REVNG_FULL_PIPELINE);
  if (written < 0 || written >= (int) sizeof(pipeline_option))
    return 2;
  const char *initialize_arguments[] = { argv[0], pipeline_option };
  if (!rp_initialize(2, initialize_arguments, 0, NULL))
    return 1;

  int result = 1;
  const uint8_t bytes[] = { 0x89, 0xf8, 0x01, 0xf0, 0xc3 };
  struct address_space space = { bytes, sizeof(bytes) };
  rp_address_space_callbacks callbacks = {
    .opaque = &space,
    .architecture = architecture,
    .entry_point = entry_point,
    .mapping_count = mapping_count,
    .mapping_at = mapping_at,
    .read = read_bytes,
  };
  rp_error *error = rp_error_create();
  rp_manager *manager = rp_manager_create_from_address_space(&callbacks,
                                                             0,
                                                             0,
                                                             NULL,
                                                             "",
                                                             error);
  if (manager == NULL) {
    print_error("manager creation", error);
    goto shutdown;
  }

  const rp_primitive_type i32 = { RP_PRIMITIVE_KIND_SIGNED, 4 };
  const rp_cabi_argument arguments[] = { { "a", i32 }, { "b", i32 } };
  if (!rp_manager_set_cabi_prototype(manager,
                                     "0x400000:Code_x86_64",
                                     "SystemV_x86_64",
                                     "add",
                                     2,
                                     arguments,
                                     &i32,
                                     error)
      || !rp_manager_set_lifter_backend(manager, argv[1], error)) {
    print_error("model or backend setup", error);
    goto destroy_manager;
  }

  rp_buffer *decompiled = rp_manager_decompile_to_c(manager, error);
  if (decompiled == NULL) {
    print_error("decompilation", error);
    goto destroy_manager;
  }
  const char *data = rp_buffer_data(decompiled);
  const uint64_t size = rp_buffer_size(decompiled);
  fwrite(data, 1, size, stdout);
  result = size == 0
           || !contains(data, size, "int32_t add(int32_t a, int32_t b)");
  rp_buffer_destroy(decompiled);

destroy_manager:
  rp_manager_destroy(manager);
shutdown:
  rp_error_destroy(error);
  if (!rp_shutdown())
    result = 1;
  return result;
}
