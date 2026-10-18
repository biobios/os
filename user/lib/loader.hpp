#pragma once

#include "syscall.hpp"
#include "elf.hpp"
#include "../src/common/utils/utils.hpp"
#include "../src/common/ipc/StartupInfo.hpp"
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

// Parses the .oz_manifest section from an ELF binary
inline bool get_elf_manifest(const std::uint8_t* elf_data, std::size_t elf_size, ProcessManifest& out_manifest) {
    if (!elf_data || elf_size < sizeof(elf::Elf64_Ehdr)) return false;
    const auto* ehdr = reinterpret_cast<const elf::Elf64_Ehdr*>(elf_data);
    if (!elf::isValidElf(ehdr)) return false;

    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0 || ehdr->e_shstrndx >= ehdr->e_shnum) {
        return false;
    }

    std::uint64_t sh_table_size = static_cast<std::uint64_t>(ehdr->e_shnum) * ehdr->e_shentsize;
    if (ehdr->e_shoff > elf_size || sh_table_size > elf_size - ehdr->e_shoff) {
        return false;
    }

    // Find section header string table (.shstrtab)
    const auto* strtab_shdr = reinterpret_cast<const elf::Elf64_Shdr*>(
        elf_data + ehdr->e_shoff + static_cast<std::uint64_t>(ehdr->e_shstrndx) * ehdr->e_shentsize
    );
    if (strtab_shdr->sh_offset > elf_size || strtab_shdr->sh_size > elf_size - strtab_shdr->sh_offset) {
        return false;
    }
    const char* strtab = reinterpret_cast<const char*>(elf_data + strtab_shdr->sh_offset);

    // Look for ".oz_manifest"
    for (int i = 0; i < ehdr->e_shnum; ++i) {
        const auto* shdr = reinterpret_cast<const elf::Elf64_Shdr*>(
            elf_data + ehdr->e_shoff + static_cast<std::uint64_t>(i) * ehdr->e_shentsize
        );
        if (shdr->sh_name < strtab_shdr->sh_size) {
            const char* sname = strtab + shdr->sh_name;
            const char* target = ".oz_manifest";
            bool match = true;
            for (int k = 0; target[k] != '\0'; ++k) {
                if (sname[k] != target[k]) { match = false; break; }
            }
            if (match && sname[12] == '\0') {
                if (shdr->sh_size >= sizeof(ProcessManifest) && shdr->sh_offset <= elf_size - sizeof(ProcessManifest)) {
                    const auto* mf = reinterpret_cast<const ProcessManifest*>(elf_data + shdr->sh_offset);
                    if (mf->magic == ProcessManifest::MAGIC) {
                        out_manifest = *mf;
                        return true;
                    }
                }
            }
        }
    }
    return false;
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
    std::uint64_t staging_vaddr = 0x550000000000ULL,
    const ProcessStartupInfo* startup_info = nullptr
);

inline SpawnedProcess spawn_process(
    std::uint32_t untyped_cap,
    std::uint32_t self_vspace_cap,
    const std::uint8_t* elf_data,
    std::size_t elf_size,
    const ProcessStartupInfo* startup_info,
    std::uint64_t staging_vaddr = 0x550000000000ULL
) {
    return spawn_process(untyped_cap, self_vspace_cap, elf_data, elf_size, staging_vaddr, startup_info);
}

inline SpawnedProcess spawn_process(
    std::uint32_t untyped_cap,
    std::uint32_t self_vspace_cap,
    const std::uint8_t* elf_data,
    std::size_t elf_size,
    std::uint64_t staging_vaddr,
    const ProcessStartupInfo* startup_info
) {

    SpawnedProcess result{};

    if (!elf_data || elf_size < sizeof(elf::Elf64_Ehdr)) return result;

    const auto* ehdr = reinterpret_cast<const elf::Elf64_Ehdr*>(elf_data);
    if (!elf::isValidElf(ehdr)) {
        return result;
    }

    // Validate program header table boundaries
    if (ehdr->e_phentsize < sizeof(elf::Elf64_Phdr) || ehdr->e_phnum == 0) {
        return result;
    }
    std::uint64_t ph_table_size = static_cast<std::uint64_t>(ehdr->e_phnum) * ehdr->e_phentsize;
    if (ehdr->e_phoff > elf_size || ph_table_size > elf_size - ehdr->e_phoff) {
        return result;
    }

    // Pre-validate all PT_LOAD segments before allocating any kernel resources
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        const auto* p = reinterpret_cast<const elf::Elf64_Phdr*>(
            elf_data + ehdr->e_phoff + static_cast<std::uint64_t>(i) * ehdr->e_phentsize
        );
        if (p->p_type != elf::PT_LOAD) continue;

        if (p->p_filesz > p->p_memsz) return result;
        if (p->p_offset > elf_size || p->p_filesz > elf_size - p->p_offset) return result;
        if (p->p_vaddr + p->p_memsz < p->p_vaddr) return result;
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
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        const auto* phdr_entry = reinterpret_cast<const elf::Elf64_Phdr*>(
            elf_data + ehdr->e_phoff + static_cast<std::uint64_t>(i) * ehdr->e_phentsize
        );
        if (phdr_entry->p_type != elf::PT_LOAD) continue;

        std::uint64_t vaddr = phdr_entry->p_vaddr;
        std::uint64_t memsz = phdr_entry->p_memsz;
        std::uint64_t filesz = phdr_entry->p_filesz;
        std::uint64_t offset = phdr_entry->p_offset;

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
        if (phdr_entry->p_flags & elf::PF_W) flags |= 0x01; // Writable
        if (phdr_entry->p_flags & elf::PF_X) flags |= 0x02; // Executable
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

    result.stack_top = STACK_TOP_VADDR - 8;

    // If startup_info is provided, write it to the top of child stack via staging buffer
    if (startup_info) {
        if (sys_vspace_map_with_pagetables(self_vspace_cap, untyped_cap, stack_frame, staging_vaddr, 0x01, STACK_SIZE) == 0) {
            utils::memset(reinterpret_cast<void*>(staging_vaddr), 0, STACK_SIZE);

            // Layout at top of stack:
            // [ STACK_TOP_VADDR - info_size - 16 ]: ProcessStartupInfo struct
            // [ STACK_TOP_VADDR - info_size - 24 ]: Pointer to ProcessStartupInfo (which will be at RSP)
            std::size_t info_size = sizeof(ProcessStartupInfo);
            std::uint64_t info_offset = STACK_SIZE - info_size - 16;
            utils::memcpy(reinterpret_cast<void*>(staging_vaddr + info_offset), startup_info, info_size);

            std::uint64_t child_info_vaddr = stack_base_vaddr + info_offset;
            *reinterpret_cast<std::uint64_t*>(staging_vaddr + info_offset - 8) = child_info_vaddr;

            result.stack_top = stack_base_vaddr + info_offset - 8;

            for (std::size_t p = 0; p < STACK_SIZE / 4096; ++p) {
                sys_vspace_unmap(self_vspace_cap, staging_vaddr + p * 4096);
            }
        }
    }

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

    if (sys_tcb_configure(result.tcb_cap, result.cspace_cap, result.vspace_cap,
                          result.entry_point, result.stack_top) != 0) {
        return result;
    }


    result.success = true;
    return result;
}

} // namespace loader
} // namespace oz
