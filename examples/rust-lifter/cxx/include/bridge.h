#pragma once

#include <cstdint>

#include "rust/cxx.h"

namespace revng_rust {

struct DecodedInstruction;
struct RustBackend;

rust::String run(RustBackend &Backend, rust::Str PipelinePath,
                 rust::Str BackendName);

rust::String emit_x86_64(std::uintptr_t Output,
                         rust::Slice<const DecodedInstruction> Instructions,
                         std::uint64_t Entry);

} // namespace revng_rust
