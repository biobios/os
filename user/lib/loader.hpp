#pragma once

#include "syscall.hpp"
#include "elf.hpp"
#include "../src/common/utils/utils.hpp"
#include <cstdint>
#include <cstddef>

namespace oz {
namespace loader {

struct TarFile {
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
};

inline TarFile tar_find_file(const std::uint8_t* tar_ptr, std::size_t tar_size, const char* filename) {
    if (!tar_ptr || tar_size < 512) return {};

    std::size_t offset = 0;
    while (offset + 512 <= tar_size) {
        const char* name = reinterpret_cast<const char*>(tar_ptr + offset);
        if (name[0] == '\0') break; // end of tar

        // Parse octal size at offset 124 (11 chars)
        const char* size_str = reinterpret_cast<const char*>(tar_ptr + offset + 124);
        std::size_t file_size = 0;
        for (int i = 0; i < 11; ++i) {
            if (size_str[i] >= '0' && size_str[i] <= '7') {
                file_size = (file_size * 8) + (size_str[i] - '0');
            }
        }

        bool match = true;
        for (int i = 0; filename[i] != '\0'; ++i) {
            if (name[i] != filename[i]) {
                match = false;
                break;
            }
        }
        if (match && name[0] != '\0') {
            return {tar_ptr + offset + 512, file_size};
        }

        offset += 512 + ((file_size + 511) & ~511ULL);
    }

    return {};
}

struct SpawnedProcess {
    std::uint32_t cspace_cap{0};
    std::uint32_t vspace_cap{0};
    std::uint32_t tcb_cap{0};
    std::uint64_t entry_point{0};
    std::uint64_t stack_top{0};
    bool success{false};
};

inline SpawnedProcess spawn_process(
    std::uint32_t untyped_cap,
    std::uint32_t self_vspace_cap,
    const std::uint8_t* elf_data,
    std::size_t elf_size,
    std::uint64_t staging_vaddr = 0x550000000000ULL
) {
    (void)elf_size;
    SpawnedProcess result{};

    if (!elf_data) return result;

    const auto* ehdr = reinterpret_cast<const elf::Elf64_Ehdr*>(elf_data);
    if (!elf::isValidElf(ehdr)) {
        return result;
    }

    // 1. Create CSpace and VSpace for child
    result.cspace_cap = sys_cspace_create(untyped_cap);
    if (result.cspace_cap == static_cast<std::uint32_t>(-1)) return result;

    result.vspace_cap = sys_vspace_create(untyped_cap);
    if (result.vspace_cap == static_cast<std::uint32_t>(-1)) return result;

    // 2. Retype a TCB from UntypedMemory
    result.tcb_cap = sys_untyped_retype(untyped_cap, static_cast<std::uint32_t>(ObjectType::TCB), 0);
    if (result.tcb_cap == static_cast<std::uint32_t>(-1)) return result;

    // 3. Load PT_LOAD segments via staging buffer
    const auto* phdr = reinterpret_cast<const elf::Elf64_Phdr*>(elf_data + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type != elf::PT_LOAD) continue;

        std::uint64_t vaddr = phdr[i].p_vaddr;
        std::uint64_t memsz = phdr[i].p_memsz;
        std::uint64_t filesz = phdr[i].p_filesz;
        std::uint64_t offset = phdr[i].p_offset;

        std::uint64_t vaddr_aligned = vaddr & ~0xFFFULL;
        std::uint64_t vaddr_offset = vaddr - vaddr_aligned;
        std::uint64_t map_size = (memsz + vaddr_offset + 0xFFF) & ~0xFFFULL;
        std::uint64_t num_pages = map_size / 4096;

        std::uint32_t seg_frame = sys_frame_alloc(untyped_cap, num_pages);
        if (seg_frame == static_cast<std::uint32_t>(-1)) return result;

        // Map into parent's staging area (Read/Write = 0x01)
        if (sys_vspace_map_with_pagetables(self_vspace_cap, untyped_cap, seg_frame, staging_vaddr, 0x01, map_size) != 0) {
            return result;
        }

        // Zero out memory (handles BSS)
        utils::memset(reinterpret_cast<void*>(staging_vaddr), 0, map_size);

        // Copy file content
        if (filesz > 0) {
            utils::memcpy(reinterpret_cast<void*>(staging_vaddr + vaddr_offset),
                          elf_data + offset, filesz);
        }

        // Unmap from staging area
        for (std::size_t p = 0; p < num_pages; ++p) {
            sys_vspace_unmap(self_vspace_cap, staging_vaddr + p * 4096);
        }

        // Map into child's VSpace
        std::uint64_t flags = 0;
        if (phdr[i].p_flags & elf::PF_W) flags |= 0x01; // Writable
        if (phdr[i].p_flags & elf::PF_X) flags |= 0x02; // Executable
        if (sys_vspace_map_with_pagetables(result.vspace_cap, untyped_cap, seg_frame, vaddr_aligned, flags, map_size) != 0) {
            return result;
        }
    }

    // 4. Allocate and map user stack (2 pages = 8KB)
    constexpr std::uint64_t STACK_SIZE = 8192;
    constexpr std::uint64_t STACK_TOP_VADDR = 0x700000000000ULL;
    std::uint64_t stack_base_vaddr = STACK_TOP_VADDR - STACK_SIZE;

    std::uint32_t stack_frame = sys_frame_alloc(untyped_cap, STACK_SIZE / 4096);
    if (stack_frame == static_cast<std::uint32_t>(-1)) return result;

    if (sys_vspace_map_with_pagetables(result.vspace_cap, untyped_cap, stack_frame, stack_base_vaddr, 0x01, STACK_SIZE) != 0) {
        return result;
    }

    // 5. Insert child's VSpace into its own CSpace at Slot 0 (CAP_SELF_VSPACE)
    if (sys_cspace_cap_insert(result.cspace_cap, CAP_SELF_VSPACE, result.vspace_cap,
                              static_cast<std::uint32_t>(CapRights::ALL)) != 0) {
        return result;
    }

    // 6. Configure TCB
    result.entry_point = ehdr->e_entry;
    result.stack_top = STACK_TOP_VADDR - 8;

    if (sys_tcb_configure(result.tcb_cap, result.cspace_cap, result.vspace_cap,
                          result.entry_point, result.stack_top) != 0) {
        return result;
    }

    result.success = true;
    return result;
}

} // namespace loader
} // namespace oz
