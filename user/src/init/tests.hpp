#pragma once

#include <cstdint>
#include <cstddef>

namespace oz::init_tests {

void run_capability_semantics_tests();
void run_vfs_tests(std::uint32_t cap_vfs_send, std::uint32_t cap_vfs_recv);
void run_display_tests(std::uint32_t cap_display_send);
void run_wm_tests(std::uint32_t cap_wm_send);
void run_manifest_tests(const std::uint8_t* initrd_ptr, std::uint64_t initrd_size, std::uint32_t cons_send);
void run_cat_tests(const std::uint8_t* initrd_ptr, std::uint64_t initrd_size, std::uint32_t cons_send, std::uint32_t cap_vfs_send, std::uint32_t cap_vfs_recv);

} // namespace oz::init_tests
