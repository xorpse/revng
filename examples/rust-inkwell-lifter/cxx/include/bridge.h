#pragma once

#include <cstdint>

namespace revng_inkwell {

void tag_root(std::uintptr_t Function);
void tag_marker(std::uintptr_t Function);
void tag_csv(std::uintptr_t Global);
std::uintptr_t basic_block_id(std::uintptr_t Module, std::uint64_t Address);
std::uint16_t code_x86_64_type();
void set_block_type(std::uintptr_t Terminator, std::uint8_t Kind);

} // namespace revng_inkwell
