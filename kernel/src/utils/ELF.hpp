#pragma once

#include <cstdint>

#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wpadded"

namespace oz {
namespace elf {

using Elf64_Addr = std::uint64_t;
using Elf64_Off = std::uint64_t;
using Elf64_Half = std::uint16_t;
using Elf64_Word = std::uint32_t;
using Elf64_Sword = std::int32_t;
using Elf64_Xword = std::uint64_t;
using Elf64_Sxword = std::int64_t;

#define EI_NIDENT 16

struct Elf64_Ehdr {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

struct Elf64_Phdr {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};

// e_ident indices
constexpr int EI_MAG0 = 0;
constexpr int EI_MAG1 = 1;
constexpr int EI_MAG2 = 2;
constexpr int EI_MAG3 = 3;
constexpr int EI_CLASS = 4;
constexpr int EI_DATA = 5;

// ELF Magic Number
constexpr unsigned char ELFMAG0 = 0x7f;
constexpr unsigned char ELFMAG1 = 'E';
constexpr unsigned char ELFMAG2 = 'L';
constexpr unsigned char ELFMAG3 = 'F';

// e_ident[EI_CLASS]
constexpr unsigned char ELFCLASS64 = 2;

// p_type
constexpr Elf64_Word PT_NULL = 0;
constexpr Elf64_Word PT_LOAD = 1;
constexpr Elf64_Word PT_DYNAMIC = 2;
constexpr Elf64_Word PT_INTERP = 3;
constexpr Elf64_Word PT_NOTE = 4;
constexpr Elf64_Word PT_SHLIB = 5;
constexpr Elf64_Word PT_PHDR = 6;
constexpr Elf64_Word PT_TLS = 7;

// p_flags
constexpr Elf64_Word PF_X = 1;
constexpr Elf64_Word PF_W = 2;
constexpr Elf64_Word PF_R = 4;

inline bool isValidElf(const Elf64_Ehdr* header) {
    if (header->e_ident[EI_MAG0] != ELFMAG0 ||
        header->e_ident[EI_MAG1] != ELFMAG1 ||
        header->e_ident[EI_MAG2] != ELFMAG2 ||
        header->e_ident[EI_MAG3] != ELFMAG3) {
        return false;
    }
    if (header->e_ident[EI_CLASS] != ELFCLASS64) {
        return false;
    }
    return true;
}

} // namespace elf
} // namespace oz

#pragma GCC diagnostic pop
