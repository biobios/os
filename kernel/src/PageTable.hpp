#pragma once

#include <cstddef>
#include <cstdint>
#include "Address.hpp"

#include "FlagClass.hpp"

namespace oz {

class PageFlags : public utils::FlagClass<std::uint64_t, PageFlags> {
public:
    constexpr explicit PageFlags(std::uint64_t flags) : FlagClass(flags) {}

    static const PageFlags None;
    static const PageFlags Present;
    static const PageFlags Writable;
    static const PageFlags User;
    static const PageFlags WriteThrough;
    static const PageFlags CacheDisable;
    static const PageFlags Accessed;
    static const PageFlags Dirty;
    static const PageFlags HugePage;
    static const PageFlags Global;
    static const PageFlags NoExecute;
};

inline constexpr PageFlags PageFlags::None         = PageFlags(0);
inline constexpr PageFlags PageFlags::Present      = PageFlags(1ULL << 0);
inline constexpr PageFlags PageFlags::Writable     = PageFlags(1ULL << 1);
inline constexpr PageFlags PageFlags::User         = PageFlags(1ULL << 2);
inline constexpr PageFlags PageFlags::WriteThrough = PageFlags(1ULL << 3);
inline constexpr PageFlags PageFlags::CacheDisable = PageFlags(1ULL << 4);
inline constexpr PageFlags PageFlags::Accessed     = PageFlags(1ULL << 5);
inline constexpr PageFlags PageFlags::Dirty        = PageFlags(1ULL << 6);
inline constexpr PageFlags PageFlags::HugePage     = PageFlags(1ULL << 7);
inline constexpr PageFlags PageFlags::Global       = PageFlags(1ULL << 8);
inline constexpr PageFlags PageFlags::NoExecute    = PageFlags(1ULL << 63);

struct PageTableEntry {
    std::uint64_t value;

    static constexpr std::uint64_t PHYSICAL_ADDRESS_MASK = 0x000F'FFFF'FFFF'F000ULL;
    static constexpr std::uint64_t FLAGS_MASK = ~PHYSICAL_ADDRESS_MASK;

    constexpr PageTableEntry() : value(0) {}
    constexpr explicit PageTableEntry(std::uint64_t v) : value(v) {}

    constexpr bool isPresent() const { return (value & static_cast<std::uint64_t>(PageFlags::Present)) != 0; }
    constexpr bool isPresent() const volatile { return (value & static_cast<std::uint64_t>(PageFlags::Present)) != 0; }

    constexpr bool isWritable() const { return (value & static_cast<std::uint64_t>(PageFlags::Writable)) != 0; }
    constexpr bool isWritable() const volatile { return (value & static_cast<std::uint64_t>(PageFlags::Writable)) != 0; }

    constexpr bool isUser() const { return (value & static_cast<std::uint64_t>(PageFlags::User)) != 0; }
    constexpr bool isUser() const volatile { return (value & static_cast<std::uint64_t>(PageFlags::User)) != 0; }

    constexpr bool isWriteThrough() const { return (value & static_cast<std::uint64_t>(PageFlags::WriteThrough)) != 0; }
    constexpr bool isWriteThrough() const volatile { return (value & static_cast<std::uint64_t>(PageFlags::WriteThrough)) != 0; }

    constexpr bool isCacheDisabled() const { return (value & static_cast<std::uint64_t>(PageFlags::CacheDisable)) != 0; }
    constexpr bool isCacheDisabled() const volatile { return (value & static_cast<std::uint64_t>(PageFlags::CacheDisable)) != 0; }

    constexpr bool isAccessed() const { return (value & static_cast<std::uint64_t>(PageFlags::Accessed)) != 0; }
    constexpr bool isAccessed() const volatile { return (value & static_cast<std::uint64_t>(PageFlags::Accessed)) != 0; }

    constexpr bool isDirty() const { return (value & static_cast<std::uint64_t>(PageFlags::Dirty)) != 0; }
    constexpr bool isDirty() const volatile { return (value & static_cast<std::uint64_t>(PageFlags::Dirty)) != 0; }

    constexpr bool isHuge() const { return (value & static_cast<std::uint64_t>(PageFlags::HugePage)) != 0; }
    constexpr bool isHuge() const volatile { return (value & static_cast<std::uint64_t>(PageFlags::HugePage)) != 0; }

    constexpr bool isGlobal() const { return (value & static_cast<std::uint64_t>(PageFlags::Global)) != 0; }
    constexpr bool isGlobal() const volatile { return (value & static_cast<std::uint64_t>(PageFlags::Global)) != 0; }

    constexpr bool isNoExecute() const { return (value & static_cast<std::uint64_t>(PageFlags::NoExecute)) != 0; }
    constexpr bool isNoExecute() const volatile { return (value & static_cast<std::uint64_t>(PageFlags::NoExecute)) != 0; }

    constexpr std::uintptr_t getAddress() const {
        return value & PHYSICAL_ADDRESS_MASK;
    }
    constexpr std::uintptr_t getAddress() const volatile {
        return value & PHYSICAL_ADDRESS_MASK;
    }

    constexpr PageFlags getFlags() const {
        return static_cast<PageFlags>(value & FLAGS_MASK);
    }
    constexpr PageFlags getFlags() const volatile {
        return static_cast<PageFlags>(value & FLAGS_MASK);
    }

    constexpr void setAddress(std::uintptr_t phys_addr) {
        value = (value & ~PHYSICAL_ADDRESS_MASK) | (phys_addr & PHYSICAL_ADDRESS_MASK);
    }
    constexpr void setAddress(std::uintptr_t phys_addr) volatile {
        value = (value & ~PHYSICAL_ADDRESS_MASK) | (phys_addr & PHYSICAL_ADDRESS_MASK);
    }

    template <typename T>
    constexpr void setAddress(PhysicalAddress<T> phys_addr) {
        setAddress(phys_addr.get());
    }
    template <typename T>
    constexpr void setAddress(PhysicalAddress<T> phys_addr) volatile {
        setAddress(phys_addr.get());
    }

    constexpr void setFlags(PageFlags flags) {
        value |= static_cast<std::uint64_t>(flags);
    }
    constexpr void setFlags(PageFlags flags) volatile {
        value |= static_cast<std::uint64_t>(flags);
    }

    constexpr void clearFlags(PageFlags flags) {
        value &= ~static_cast<std::uint64_t>(flags);
    }
    constexpr void clearFlags(PageFlags flags) volatile {
        value &= ~static_cast<std::uint64_t>(flags);
    }

    constexpr void set(std::uintptr_t phys_addr, PageFlags flags) {
        value = (phys_addr & PHYSICAL_ADDRESS_MASK) | static_cast<std::uint64_t>(flags);
    }
    constexpr void set(std::uintptr_t phys_addr, PageFlags flags) volatile {
        value = (phys_addr & PHYSICAL_ADDRESS_MASK) | static_cast<std::uint64_t>(flags);
    }

    template <typename T>
    constexpr void set(PhysicalAddress<T> phys_addr, PageFlags flags) {
        set(phys_addr.get(), flags);
    }
    template <typename T>
    constexpr void set(PhysicalAddress<T> phys_addr, PageFlags flags) volatile {
        set(phys_addr.get(), flags);
    }

    constexpr void clear() {
        value = 0;
    }
    constexpr void clear() volatile {
        value = 0;
    }

    constexpr std::uint64_t raw() const { return value; }
    constexpr std::uint64_t raw() const volatile { return value; }
    constexpr void setRaw(std::uint64_t v) { value = v; }
    constexpr void setRaw(std::uint64_t v) volatile { value = v; }
};

static_assert(sizeof(PageTableEntry) == 8, "PageTableEntry must be 8 bytes");

struct alignas(4096) PageTable {
    static constexpr std::size_t ENTRY_COUNT = 512;
    PageTableEntry entries[ENTRY_COUNT];

    constexpr PageTableEntry& operator[](std::size_t index) {
        return entries[index];
    }

    constexpr const PageTableEntry& operator[](std::size_t index) const {
        return entries[index];
    }

    constexpr volatile PageTableEntry& operator[](std::size_t index) volatile {
        return entries[index];
    }

    constexpr const volatile PageTableEntry& operator[](std::size_t index) const volatile {
        return entries[index];
    }

    void clear() {
        for (std::size_t i = 0; i < ENTRY_COUNT; ++i) {
            entries[i].clear();
        }
    }

    void clear() volatile {
        for (std::size_t i = 0; i < ENTRY_COUNT; ++i) {
            entries[i].clear();
        }
    }
};

static_assert(sizeof(PageTable) == 4096, "PageTable must be 4096 bytes");
static_assert(alignof(PageTable) == 4096, "PageTable must be 4096-byte aligned");

using PML4Table = PageTable;
using PDPT = PageTable;
using PageDirectory = PageTable;
using PT = PageTable;

namespace paging {
    constexpr std::size_t pml4Index(std::uintptr_t virt_addr) {
        return (virt_addr >> 39) & 0x1FF;
    }

    constexpr std::size_t pdptIndex(std::uintptr_t virt_addr) {
        return (virt_addr >> 30) & 0x1FF;
    }

    constexpr std::size_t pdIndex(std::uintptr_t virt_addr) {
        return (virt_addr >> 21) & 0x1FF;
    }

    constexpr std::size_t ptIndex(std::uintptr_t virt_addr) {
        return (virt_addr >> 12) & 0x1FF;
    }

    constexpr std::size_t pageOffset(std::uintptr_t virt_addr) {
        return virt_addr & 0xFFF;
    }
} // namespace paging

} // namespace oz
