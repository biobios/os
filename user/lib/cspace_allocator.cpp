#include "cspace_allocator.hpp"

namespace oz {

constinit CSpaceSlotAllocator g_slot_allocator;

void CSpaceSlotAllocator::reset() {
    UserScopedLock guard(lock_);
    for (std::size_t i = 0; i < WORDS; ++i) bitmap_[i] = 0;

    // 1. スロット 0..127 (Null, Initial Endpoints 1..63, PT_CACHE 64..127) の予約
    for (std::size_t i = 0; i <= CAP_PT_DELEGATE_END; ++i) {
        mark_used(i);
    }

    // 2. スロット 200..249 (System Untyped & Well-known スロット) の静的予約
    for (std::size_t i = CAP_RESERVED_SYSTEM_START; i <= CAP_RESERVED_SYSTEM_END; ++i) {
        mark_used(i);
    }
}

void CSpaceSlotAllocator::reserve_slot(std::uint32_t slot) {
    if (slot < MAX_SLOTS) {
        UserScopedLock guard(lock_);
        mark_used(slot);
    }
}

void CSpaceSlotAllocator::reserve_range(std::uint32_t start_slot, std::size_t count) {
    if (start_slot >= MAX_SLOTS || count == 0 || count > MAX_SLOTS - start_slot) return;
    UserScopedLock guard(lock_);
    for (std::size_t i = 0; i < count; ++i) {
        mark_used(start_slot + i);
    }
}

std::uint32_t CSpaceSlotAllocator::allocate_one() {
    UserScopedLock guard(lock_);
    for (std::size_t w = 0; w < WORDS; ++w) {
        std::uint64_t inv = ~bitmap_[w];
        if (inv == 0) continue;
        int bit = __builtin_ctzll(inv);
        std::size_t slot = w * 64 + bit;
        if (slot >= MAX_SLOTS) return INVALID_CAP_ID;
        bitmap_[w] |= (1ULL << bit);
        return static_cast<std::uint32_t>(slot);
    }
    return INVALID_CAP_ID;
}

std::uint32_t CSpaceSlotAllocator::allocate_contiguous(std::size_t count) {
    if (count == 0 || count > MAX_SLOTS) return INVALID_CAP_ID;
    if (count == 1) return allocate_one();

    UserScopedLock guard(lock_);

    std::size_t consecutive = 0;
    std::size_t start_idx = 0;

    for (std::size_t i = 1; i < MAX_SLOTS; ++i) {
        if (is_reserved_slot(i)) {
            consecutive = 0;
            continue;
        }

        if (!is_used(i)) {
            if (consecutive == 0) start_idx = i;
            consecutive++;
            if (consecutive == count) {
                for (std::size_t k = 0; k < count; ++k) {
                    mark_used(start_idx + k);
                }
                return static_cast<std::uint32_t>(start_idx);
            }
        } else {
            consecutive = 0;
        }
    }
    return INVALID_CAP_ID;
}

void CSpaceSlotAllocator::free(std::uint32_t slot, std::size_t count) {
    if (slot == 0 || count == 0 || slot >= MAX_SLOTS || count > MAX_SLOTS - slot) return;
    UserScopedLock guard(lock_);
    for (std::size_t k = 0; k < count; ++k) {
        std::uint32_t s = slot + static_cast<std::uint32_t>(k);
        if (is_reserved_slot(s)) continue;
        clear_used(s);
    }
}

bool CSpaceSlotAllocator::is_used(std::size_t slot) const {
    return (bitmap_[slot / 64] & (1ULL << (slot % 64))) != 0;
}

} // namespace oz
