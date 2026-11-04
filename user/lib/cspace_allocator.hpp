#pragma once

#include "spinlock.hpp"
#include <oz/CapabilityTypes.hpp>
#include <cstdint>
#include <cstddef>

namespace oz {

class CSpaceSlotAllocator {
public:
    static constexpr std::size_t MAX_SLOTS = 4096;
    static constexpr std::size_t WORDS = MAX_SLOTS / 64; // 64 words (512 bytes)

    constexpr CSpaceSlotAllocator() {
        bitmap_[0] = ~0ULL; // Word 0: bits 0..63 (Null, Initial Endpoints & Prewired 1..63)
        bitmap_[1] = ~0ULL; // Word 1: bits 64..127 (PT_CACHE delegation slots 64..127)
        bitmap_[3] = 0x03FFFFFFFFFFFF00ULL; // Word 3: bits 8..57 (slots 200..249: CAP_RESERVED_SYSTEM_START..CAP_RESERVED_SYSTEM_END)
    }

    static constexpr bool is_reserved_slot(std::size_t slot) {
        return (slot <= CAP_PT_DELEGATE_END) || is_reserved_system_slot(static_cast<CapID>(slot));
    }

    void reset();
    void reserve_slot(std::uint32_t slot);
    void reserve_range(std::uint32_t start_slot, std::size_t count);
    std::uint32_t allocate_one();
    std::uint32_t allocate_contiguous(std::size_t count);
    void free(std::uint32_t slot, std::size_t count = 1);
    bool is_used(std::size_t slot) const;

private:
    void mark_used(std::size_t slot) {
        bitmap_[slot / 64] |= (1ULL << (slot % 64));
    }

    void clear_used(std::size_t slot) {
        bitmap_[slot / 64] &= ~(1ULL << (slot % 64));
    }

    UserSpinlock  lock_{};
    std::uint64_t bitmap_[WORDS]{
        ~0ULL, // Word 0: bits 0..63 (Null, Initial Endpoints & Prewired 1..63)
        ~0ULL, // Word 1: bits 64..127 (PT_CACHE delegation slots 64..127)
        0,
        0x03FFFFFFFFFFFF00ULL, // Word 3: bits 8..57 (slots 200..249: CAP_RESERVED_SYSTEM_START..CAP_RESERVED_SYSTEM_END)
        0
    };
};

extern CSpaceSlotAllocator g_slot_allocator;

} // namespace oz
