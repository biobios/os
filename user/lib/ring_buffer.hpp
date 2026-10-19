#pragma once
#include <cstdint>
#include <cstddef>

namespace oz {

static constexpr std::uint32_t RING_FLAG_NEED_SIGNAL = 1 << 0;

template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static constexpr std::size_t MASK = Capacity - 1;

public:
    // 64-byte alignment to prevent false sharing between producer and consumer cores
    alignas(64) volatile std::uint32_t head{0};
    alignas(64) volatile std::uint32_t tail{0};
    alignas(64) volatile std::uint32_t flags{0};
    alignas(64) T entries[Capacity]{};

    bool is_empty() const volatile {
        std::uint32_t h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
        std::uint32_t t = __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
        return h == t;
    }

    bool is_full() const volatile {
        std::uint32_t h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
        std::uint32_t t = __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
        return (t - h) >= Capacity;
    }

    std::size_t size() const volatile {
        std::uint32_t h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
        std::uint32_t t = __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
        return t - h;
    }

    // Producer: Enqueue item. Returns true if queued.
    // If need_signal != nullptr, sets whether producer should signal consumer.
    bool push(const T& item, bool* need_signal = nullptr) volatile {
        std::uint32_t t = __atomic_load_n(&tail, __ATOMIC_RELAXED);
        std::uint32_t h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);

        if ((t - h) >= Capacity) {
            return false; // Queue full
        }

        const_cast<T&>(entries[t & MASK]) = item;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&tail, t + 1, __ATOMIC_RELEASE);

        if (need_signal) {
            std::uint32_t f = __atomic_load_n(&flags, __ATOMIC_ACQUIRE);
            *need_signal = ((f & RING_FLAG_NEED_SIGNAL) != 0) || (t == h);
        }
        return true;
    }

    // Consumer: Dequeue item. Returns true if an item was read.
    bool pop(T* item) volatile {
        std::uint32_t h = __atomic_load_n(&head, __ATOMIC_RELAXED);
        std::uint32_t t = __atomic_load_n(&tail, __ATOMIC_ACQUIRE);

        if (h == t) {
            return false; // Queue empty
        }

        if (item) {
            *item = const_cast<const T&>(entries[h & MASK]);
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&head, h + 1, __ATOMIC_RELEASE);
        return true;
    }

    // Consumer indicates whether it is sleeping / waiting on Notification
    void set_need_signal(bool need) volatile {
        if (need) {
            __atomic_fetch_or(&flags, RING_FLAG_NEED_SIGNAL, __ATOMIC_RELEASE);
        } else {
            __atomic_fetch_and(&flags, ~RING_FLAG_NEED_SIGNAL, __ATOMIC_RELEASE);
        }
    }
};

// ============================================================================
// Block I/O Async Ring Definitions (for VFS <-> AHCI)
// ============================================================================

enum class AsyncBlockCmd : std::uint32_t {
    READ     = 1,
    WRITE    = 2,
    GET_INFO = 3,
};

struct AsyncBlockReq {
    std::uint64_t req_id{0};       // Caller-defined identifier for callback
    std::uint32_t cmd{0};          // AsyncBlockCmd
    std::uint32_t count{0};        // Sector count
    std::uint64_t lba{0};          // Start LBA
    std::uint64_t buf_offset{0};   // Offset in shared DMA buffer
};

struct AsyncBlockResp {
    std::uint64_t req_id{0};       // Corresponds to req_id
    std::int32_t  status{0};       // 0 = OK, negative = error
    std::uint32_t info{0};         // Sector size (for GET_INFO) or sectors transferred
};

struct BlockIORing {
    SPSCQueue<AsyncBlockReq, 32>  sq; // Request queue: VFS -> AHCI
    SPSCQueue<AsyncBlockResp, 32> cq; // Response queue: AHCI -> VFS
};

// ============================================================================
// GUI / WM Input Event Ring Definitions (for WM / Input stream)
// ============================================================================

struct WMInputEvent {
    std::uint32_t type{0};         // WMEventType
    std::uint32_t code{0};         // Key code or mouse button
    std::int32_t  x{0};            // Relative or absolute X
    std::int32_t  y{0};            // Relative or absolute Y
    std::uint32_t modifiers{0};    // Shift / Ctrl / Alt
    std::uint32_t reserved{0};
};

using WMInputRing = SPSCQueue<WMInputEvent, 64>;

} // namespace oz
