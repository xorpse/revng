//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <stdint.h>
#include <stdio.h>

#include "revng/PipelineC/PipelineC.h"

struct address_space {
  uint8_t bytes[2];
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

static bool
mapping_at(void *opaque, uint64_t index, rp_address_space_mapping *output) {
  struct address_space *space = opaque;
  if (index != 0)
    return false;
  output->start = "0x400000:Code_x86_64";
  output->virtual_size = 4;
  output->contents = space->bytes;
  output->contents_size = sizeof(space->bytes);
  output->readable = true;
  output->writeable = false;
  output->executable = true;
  output->name = "example-code";
  return true;
}

static uint64_t extra_code_count(void *opaque) {
  (void) opaque;
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s {libtcg|reference-x86_64}\n", argv[0]);
    return 2;
  }

  char pipeline_option[4096];
  int written = snprintf(pipeline_option,
                         sizeof(pipeline_option),
                         "--pipeline-path=%s",
                         REVNG_TEST_PIPELINE);
  if (written >= (int) sizeof(pipeline_option))
    return 2;
  const char *initialize_arguments[] = { argv[0], pipeline_option };
  if (!rp_initialize(2, initialize_arguments, 0, NULL))
    return 1;

  int result = 1;
  struct address_space space = { { 0x90, 0xc3 } };
  rp_address_space_callbacks callbacks = { &space,      architecture,
                                           entry_point, mapping_count,
                                           mapping_at,  extra_code_count,
                                           NULL };
  rp_error *error = rp_error_create();
  rp_manager *manager = rp_manager_create_from_address_space(&callbacks,
                                                             0,
                                                             NULL,
                                                             "",
                                                             error);
  if (manager == NULL)
    goto shutdown;
  if (!rp_manager_set_lifter_backend(manager, argv[1], error))
    goto destroy_manager;

  rp_step *step = rp_manager_get_step_from_name(manager, "lift");
  const rp_container_identifier
    *identifier = rp_manager_get_container_identifier_from_name(manager,
                                                                "root.bc.zstd");
  const rp_kind *kind = rp_manager_get_kind_from_name(manager, "root");
  if (step == NULL || identifier == NULL || kind == NULL)
    goto destroy_manager;
  rp_container *container = rp_step_get_container(step, identifier);
  const char *path[] = { NULL };
  rp_target *target = rp_target_create(kind, 0, path);
  const rp_target *targets[] = { target };
  rp_buffer *module = rp_manager_produce_targets(manager,
                                                 step,
                                                 container,
                                                 1,
                                                 targets,
                                                 error);
  rp_target_destroy(target);
  if (module == NULL)
    goto destroy_manager;

  printf("%s produced %llu bytes of LLVM module\n",
         argv[1],
         (unsigned long long) rp_buffer_size(module));
  result = rp_buffer_size(module) == 0;
  rp_buffer_destroy(module);

destroy_manager:
  rp_manager_destroy(manager);
shutdown:
  rp_error_destroy(error);
  if (!rp_shutdown())
    result = 1;
  return result;
}
