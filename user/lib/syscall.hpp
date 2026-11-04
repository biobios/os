#pragma once
#include <cstdint>
#include <cstddef>

namespace oz {

enum class CapRights : std::uint32_t {
    NONE        = 0,

    // Common Rights (bit 0..15)
    DUPLICATE   = 1U << 0,
    TRANSFER    = 1U << 1,
    DERIVE      = 1U << 2,
    REVOKE      = 1U << 3,

    // Type Rights (bit 16..31)
    READ        = 1U << 16,
    RECEIVE     = 1U << 16,
    WAIT        = 1U << 16,
    CONFIGURE   = 1U << 16,
    SWITCH      = 1U << 16,
    IN          = 1U << 16,
    INSERT      = 1U << 16,

    WRITE       = 1U << 17,
    SEND        = 1U << 17,
    SIGNAL      = 1U << 17,
    CONTROL     = 1U << 17,
    OUT         = 1U << 17,
    ACK_WAIT    = 1U << 17,
    DELETE      = 1U << 17,

    EXEC        = 1U << 18,
    GRANT       = 1U << 18,
    BIND        = 1U << 18,
    SET_BUDGET  = 1U << 18,

    NOCACHE     = 1U << 19,
    GRANT_REPLY = 1U << 19,
    DEBUG       = 1U << 19,

    USE         = 1U << 4, // 互換用

    ALL         = 0xFFFFFFFFU,
};

inline constexpr CapRights operator|(CapRights a, CapRights b) {
    return static_cast<CapRights>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline constexpr CapRights operator&(CapRights a, CapRights b) {
    return static_cast<CapRights>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

inline constexpr CapRights operator~(CapRights a) {
    return static_cast<CapRights>(~static_cast<std::uint32_t>(a));
}

enum class ObjectType : std::uint32_t {
    Unknown = 0,
    ChannelEndpoint = 1,
    Frame = 2,
    UntypedMemory = 3,
    VSpace = 4,
    TCB = 5,
    CSpaceCap = 6,
    IRQHandler = 7,
    IOPort = 8,
    PageTable = 9,
    Notification = 10,
    Baton = 11,
    IRQBatonControl = 12,
};

constexpr int SYS_ERR_INVALID      = -1;
constexpr int SYS_ERR_WOULDBLOCK   = -2;
constexpr int SYS_ERR_CANCELLED    = -3;
constexpr int SYS_ERR_DEADLK       = -4;
constexpr int SYS_ERR_INVALID_TYPE = -5;
constexpr int SYS_ERR_BUSY         = -6;
constexpr int SYS_ERR_INVALID_STATE = -7;
constexpr int SYS_ERR_PERM          = -8;
constexpr int SYS_ERR_NO_MEMORY     = -9;

constexpr std::uint64_t TIMEOUT_BADGE          = 1ULL << 61;
constexpr std::uint64_t BUDGET_EXHAUSTED_BADGE = 1ULL << 62;

enum class ChannelMode : std::uint8_t {
    BLOCKING        = 0, // 送信側: ブロッキング / 受信側: ブロッキング (従来互換)
    RECV_NONBLOCK   = 1, // 送信側: ブロッキング / 受信側: ノンブロッキング
    SEND_NONBLOCK   = 2, // 送信側: ノンブロッキング / 受信側: ブロッキング
};

constexpr std::uint32_t IPC_FLAG_BLOCK = 1;

enum class BatonState : std::uint8_t {
    READY       = 0,
    RUNNING     = 1,
    WAIT_RETURN = 2,
    WAIT_START  = 3,
    BLOCKED     = 4
};

enum class SchedReason : std::uint64_t {
    NONE      = 0,
    TIMEOUT   = 1,
    YIELD     = 2,
    BLOCKED   = 3,
    EXIT      = 4,
    PREEMPTED = 5,
    FAULT     = 6,
    ORPHANED  = 7
};

constexpr SchedReason SYS_BATON_SWITCH_TIMEOUT = SchedReason::TIMEOUT;
constexpr SchedReason SYS_BATON_SWITCH_INVALID = static_cast<SchedReason>(SYS_ERR_INVALID);

struct SchedResult {
    SchedReason reason{SchedReason::NONE};
    union {
        std::uint64_t consumed_cycles{0}; // PREEMPTED時は時間切れとなったirq_vector
        std::uint64_t consumed_us;
    };
};

using ReplyToken = std::uint64_t;
constexpr ReplyToken INVALID_REPLY_TOKEN = 0;
using CapID = std::uint32_t;
static constexpr std::uint32_t INVALID_CAP_ID = 0xFFFFFFFF;

// Reserved slot range (slots 200..249: 50 slots):
// Reserved for system use: slots 200..239 are reserved for system UntypedMemory pools and internal buffers,
// and slots 240..249 are well-known capabilities (CAP_UNTYPED, CAP_BOOTINFO, CAP_SELF_CSPACE, etc.).
// Dynamic user allocations avoid this range to prevent collision with system initialization.
constexpr CapID CAP_PREWIRED_START        = 1;
constexpr CapID CAP_PREWIRED_END          = 63;
constexpr CapID CAP_PT_DELEGATE_START     = 64;
constexpr CapID CAP_PT_DELEGATE_END       = 127;
constexpr CapID CAP_RESERVED_SYSTEM_START = 200;
constexpr CapID CAP_RESERVED_SYSTEM_END   = 249;

inline bool is_reserved_system_slot(CapID slot) {
    return (slot >= CAP_RESERVED_SYSTEM_START && slot <= CAP_RESERVED_SYSTEM_END);
}

// ユーザー空間向け軽量スピンロック (freestanding 組み込み関数利用)
class UserSpinlock {
private:
    char flag_{0};
public:
    void lock() {
        while (__atomic_test_and_set(&flag_, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("pause");
        }
    }
    void unlock() {
        __atomic_clear(&flag_, __ATOMIC_RELEASE);
    }
};

class UserScopedLock {
private:
    UserSpinlock& lock_;
public:
    explicit UserScopedLock(UserSpinlock& l) : lock_(l) { lock_.lock(); }
    ~UserScopedLock() { lock_.unlock(); }
};

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

    void reset() {
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

    void reserve_slot(std::uint32_t slot) {
        if (slot < MAX_SLOTS) {
            UserScopedLock guard(lock_);
            mark_used(slot);
        }
    }

    void reserve_range(std::uint32_t start_slot, std::size_t count) {
        if (start_slot >= MAX_SLOTS || count == 0 || count > MAX_SLOTS - start_slot) return;
        UserScopedLock guard(lock_);
        for (std::size_t i = 0; i < count; ++i) {
            mark_used(start_slot + i);
        }
    }

    std::uint32_t allocate_one() {
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

    std::uint32_t allocate_contiguous(std::size_t count) {
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

    void free(std::uint32_t slot, std::size_t count = 1) {
        if (slot == 0 || count == 0 || slot >= MAX_SLOTS || count > MAX_SLOTS - slot) return;
        UserScopedLock guard(lock_);
        for (std::size_t k = 0; k < count; ++k) {
            std::uint32_t s = slot + static_cast<std::uint32_t>(k);
            if (is_reserved_slot(s)) continue;
            clear_used(s);
        }
    }

    bool is_used(std::size_t slot) const {
        return (bitmap_[slot / 64] & (1ULL << (slot % 64))) != 0;
    }

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

struct IPCMessage {
    std::uint64_t data[4];
    std::uint32_t transferred_cap;
    bool has_cap{false};
    std::uint64_t badge{0};
};

struct CapInfo {
    std::uint8_t  type{0};
    std::uint8_t  level{0};
    std::uint8_t  is_mapped{0};
    std::uint8_t  padding{0};
    std::uint32_t rights{0};
    std::uint64_t size{0};
};

enum class KernelCapType : std::uint8_t {
    Null            = 0,
    UntypedMemory   = 1,
    CSpace          = 2,
    TCB             = 3,
    VSpace          = 4,
    Frame           = 5,
    PageTable       = 6,
    Notification    = 7,
    ChannelEndpoint = 8,
    IOPort          = 9,
    Baton           = 10,
    IRQBatonControl = 11,
};
constexpr std::uint8_t CAP_TYPE_FRAME     = static_cast<std::uint8_t>(KernelCapType::Frame);
constexpr std::uint8_t CAP_TYPE_PAGETABLE = static_cast<std::uint8_t>(KernelCapType::PageTable);

struct PtCacheEntry {
    std::uint32_t vspace_cap{0};
    std::uint32_t pt_cap{0};
    std::uint8_t  level{0}; // 1: PT, 2: PD, 3: PDPT
    bool          valid{false};
    std::uint64_t va_prefix{0};
};
static constexpr std::size_t PT_CACHE_SIZE = 1024;

extern "C" {

extern PtCacheEntry g_pt_cache[PT_CACHE_SIZE];

std::uint64_t syscall(std::uint64_t number, std::uint64_t arg1, std::uint64_t arg2, std::uint64_t arg3, std::uint64_t arg4, std::uint64_t arg5, std::uint64_t arg6);
void sys_exit(int status);
int sys_yield();
std::size_t sys_write(const char* str);

// Bulk untyped retype (syscall 59)
int sys_untyped_retype_bulk(std::uint32_t untyped_cap, ObjectType target_type, std::uint64_t size_or_arg, std::uint32_t target_cspace, std::uint32_t start_slot, std::size_t count);

// Channel management (explicit slot syscall 3)
int sys_channel_create(std::uint32_t untyped_cap,
                       std::uint32_t target_cs0, std::uint32_t slot0,
                       std::uint32_t target_cs1, std::uint32_t slot1,
                       ChannelMode mode0 = ChannelMode::BLOCKING,
                       ChannelMode mode1 = ChannelMode::BLOCKING);

// IPC calls (syscall 27 & 28) with recv_slot
constexpr std::uint32_t IPC_RECV_SLOT_NONE = 0xFFFFFFFFU;
bool sys_call(std::uint32_t cap_id, const IPCMessage* send_msg, IPCMessage* recv_msg, std::uint32_t recv_slot = 0);
bool sys_call_recv_cap(std::uint32_t cap_id, const IPCMessage* send_msg, IPCMessage* recv_msg, std::uint32_t* out_slot = nullptr);
int sys_call_ext(std::uint32_t cap_id, const IPCMessage* send_msg, IPCMessage* recv_msg, std::uint32_t flags = 0, std::uint32_t recv_slot = 0);
int sys_reply_and_recv(std::uint32_t ep_cap, const IPCMessage* reply_msg, IPCMessage* recv_msg, std::uint32_t flags = 0, std::uint32_t recv_slot = 0);
ReplyToken sys_reply_recv(ReplyToken reply_token, std::uint32_t ep_cap, const IPCMessage* reply_msg, IPCMessage* recv_msg, std::uint32_t recv_slot = 0);

// Capability-based memory management
int sys_pagetable_create(std::uint32_t untyped_cap, std::uint32_t frame_cap, std::uint32_t level, std::uint32_t target_cspace, std::uint32_t target_slot);
int sys_pagetable_map(std::uint32_t parent_pt_cap, std::uint32_t index, std::uint32_t child_pt_cap);
int sys_pagetable_map_frame(std::uint32_t pt_cap, std::uint32_t index, std::uint32_t frame_cap, std::uint64_t flags);
int sys_cap_unmap(std::uint32_t mapped_cap);
int sys_cap_identify(std::uint32_t cap_id, CapInfo* info);

// Helper functions for hierarchical page table management
std::uint32_t sys_pagetable_alloc(std::uint32_t untyped_cap, std::uint32_t level);
std::uint32_t sys_alloc_shared_pt(std::uint32_t untyped_cap, std::size_t num_pages, std::uint64_t flags);
std::uint32_t sys_ensure_intermediate_pt(std::uint32_t vspace_cap, std::uint32_t untyped_cap, std::uint64_t virt_addr, std::uint32_t target_level);

int sys_vspace_map(std::uint32_t vspace_cap, std::uint32_t frame_cap,
                   std::uint64_t virt_addr, std::uint64_t flags);
int sys_vspace_map_pagetable(std::uint32_t vspace_cap, std::uint32_t pagetable_cap,
                             std::uint64_t virt_addr, std::uint32_t level);
int sys_vspace_ensure_pagetables_range(std::uint32_t vspace_cap, std::uint32_t untyped_cap,
                                       std::uint64_t start_vaddr, std::uint64_t size);
int sys_vspace_map_with_pagetables(std::uint32_t vspace_cap, std::uint32_t untyped_cap,
                                   std::uint32_t frame_cap, std::uint64_t virt_addr,
                                   std::uint64_t flags, std::uint64_t size = 4096);
[[deprecated("Use sys_cap_unmap(mapped_cap) instead")]]
int sys_vspace_unmap(std::uint32_t vspace_cap, std::uint64_t virt_addr);
std::uint64_t sys_frame_phys_addr(std::uint32_t frame_cap);
std::uint64_t sys_frame_size(std::uint32_t frame_cap);

int sys_untyped_revoke(std::uint32_t untyped_cap);

void sys_debug_print(const char* str);

// Capability management
int sys_cap_mint(std::uint32_t target_cspace, std::uint32_t target_slot, std::uint32_t src_cap, std::uint32_t new_rights, std::uint64_t badge = 0);
int sys_cap_revoke(std::uint32_t cap_id);
int sys_cap_delete(std::uint32_t cap_id);
int sys_cap_move(std::uint32_t dest_slot, std::uint32_t src_slot);
int sys_cspace_cap_move(std::uint32_t target_cspace_cap, std::uint32_t target_slot, std::uint32_t src_slot);
int sys_cap_copy(std::uint32_t dest_slot, std::uint32_t src_slot, std::uint32_t rights);

// Process/thread management
int sys_tcb_configure(std::uint32_t tcb_cap, std::uint32_t cspace_cap,
                      std::uint32_t vspace_cap, std::uint64_t rip,
                      std::uint64_t user_rsp);
int sys_tcb_resume(std::uint32_t tcb_cap);
int sys_tcb_suspend(std::uint32_t tcb_cap);
int sys_tcb_set_fault_endpoint(std::uint32_t tcb_cap, std::uint32_t ep_cap);
int sys_tcb_set_exit_notification(std::uint32_t tcb_cap, std::uint32_t notif_cap, std::uint64_t badge);
int sys_tcb_cancel_ipc(std::uint32_t tcb_cap);
int sys_tcb_get_cpu_time(std::uint32_t tcb_cap, std::uint64_t* out_cycles);
int sys_tcb_get_id(std::uint32_t tcb_cap, std::uint64_t* out_id);
int sys_tcb_set_sched_notification(std::uint32_t tcb_cap, std::uint32_t notif_cap, std::uint64_t badge);
int sys_tcb_bind_baton(std::uint32_t tcb_cap, std::uint32_t baton_cap);
inline int sys_tcb_unbind_baton(std::uint32_t tcb_cap) {
    return sys_tcb_bind_baton(tcb_cap, 0);
}
int sys_baton_bind_notification(std::uint32_t baton_cap, std::uint32_t notif_cap, std::uint64_t badge);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type-c-linkage"
SchedResult sys_baton_switch(std::uint32_t target_baton_cap, std::uint64_t quantum_us = 0);
SchedResult sys_baton_split(std::uint32_t target_tcb_cap, std::uint64_t budget_us);
SchedResult sys_donate_time(std::uint32_t target_tcb_cap, std::uint64_t time_budget_us);
#pragma GCC diagnostic pop
[[noreturn]] inline void sys_baton_exit(int exit_code) {
    sys_exit(exit_code);
    while (1) {}
}
inline int sys_baton_yield() {
    return sys_yield();
}
int sys_baton_inject(std::uint32_t target_baton_cap, std::uint64_t budget_us);
int sys_baton_set_budget(std::uint32_t baton_cap, std::uint64_t budget_us);
int sys_baton_create_irq(std::uint32_t irq_control_cap, std::uint32_t untyped_cap, std::uint64_t initial_budget_us, std::uint32_t target_cspace, std::uint32_t target_slot);
int sys_thread_set_notification(std::uint32_t thread_cap, std::uint32_t notif_cap, std::uint64_t badge);
int sys_irq_wait(std::uint32_t irq_handler_cap, std::uint32_t irq_baton_cap);
int sys_irq_handler_set_notification(std::uint32_t irq_handler_cap, std::uint32_t notif_cap, std::uint64_t badge);
int sys_irq_ack(std::uint32_t irq_handler_cap);

int sys_cspace_cap_insert(std::uint32_t cspace_cap, std::uint32_t target_slot,
                          std::uint32_t src_cap, std::uint32_t rights);
std::uint32_t sys_vspace_create(std::uint32_t untyped_cap);

// Hardware resource management
int sys_irq_handler_create(std::uint32_t untyped_cap, std::uint8_t vector, std::uint32_t notification_ep_cap, std::uint32_t target_cspace, std::uint32_t target_slot);
int sys_ioport_create(std::uint32_t parent_ioport_cap, std::uint16_t base_port, std::uint32_t port_count, std::uint32_t target_cspace, std::uint32_t target_slot);
std::uint64_t sys_ioport_read(std::uint32_t ioport_cap, std::uint16_t port, std::uint32_t width);
int sys_ioport_write(std::uint32_t ioport_cap, std::uint16_t port, std::uint64_t value, std::uint32_t width);

// Notification management
int sys_notification_signal(std::uint32_t notif_cap, std::uint64_t badge);
int sys_notification_wait(std::uint32_t notif_cap, std::uint64_t* out_badge);
int sys_channel_bind_notification(std::uint32_t ep_cap, std::uint32_t notif_cap, std::uint64_t badge);
int sys_notification_poll(std::uint32_t notif_cap, std::uint64_t* out_badge);
int sys_notification_bind_parent(std::uint32_t child_notif_cap, std::uint32_t parent_notif_cap, std::uint64_t badge);
int sys_notification_unbind_parent(std::uint32_t child_notif_cap);

} // extern "C"

// Backward-compatible and convenience userland wrappers (inlined to avoid C linkage collision)
inline int sys_reply(const IPCMessage* reply_msg) {
    return sys_reply_and_recv(0, reply_msg, nullptr, 0);
}

inline int sys_recv(std::uint32_t ep_cap, IPCMessage* recv_msg, std::uint32_t flags = 0, std::uint32_t recv_slot = 0) {
    return sys_reply_and_recv(ep_cap, nullptr, recv_msg, flags, recv_slot);
}

inline int sys_untyped_retype(std::uint32_t untyped_cap, ObjectType target_type, std::uint64_t size_or_arg, std::uint32_t target_cspace, std::uint32_t start_slot, std::size_t count) {
    return sys_untyped_retype_bulk(untyped_cap, target_type, size_or_arg, target_cspace, start_slot, count);
}

inline std::uint32_t sys_untyped_retype(std::uint32_t untyped_cap, std::uint32_t target_type, std::uint64_t arg) {
    std::uint32_t slot = g_slot_allocator.allocate_one();
    if (slot == INVALID_CAP_ID) return INVALID_CAP_ID;

    int res = sys_untyped_retype_bulk(
        untyped_cap,
        static_cast<ObjectType>(target_type),
        arg,
        0,       // 自 CSpace
        slot,
        1        // count = 1
    );
    if (res != 0) {
        g_slot_allocator.free(slot);
        return INVALID_CAP_ID;
    }
    return slot;
}

inline bool sys_channel_create(std::uint32_t untyped_cap, std::uint32_t* out_cap0, std::uint32_t* out_cap1, ChannelMode mode0 = ChannelMode::BLOCKING, ChannelMode mode1 = ChannelMode::BLOCKING) {
    if (!out_cap0 || !out_cap1) return false;

    std::uint32_t s0 = g_slot_allocator.allocate_one();
    std::uint32_t s1 = g_slot_allocator.allocate_one();
    if (s0 == INVALID_CAP_ID || s1 == INVALID_CAP_ID) {
        if (s0 != INVALID_CAP_ID) g_slot_allocator.free(s0);
        if (s1 != INVALID_CAP_ID) g_slot_allocator.free(s1);
        return false;
    }

    int res = sys_channel_create(untyped_cap, 0, s0, 0, s1, mode0, mode1);
    if (res != 0) {
        g_slot_allocator.free(s0);
        g_slot_allocator.free(s1);
        return false;
    }

    *out_cap0 = s0;
    *out_cap1 = s1;
    return true;
}

inline std::uint32_t sys_frame_alloc(std::uint32_t untyped_cap, std::uint32_t start_slot, std::uint64_t count) {
    if (count == 0) return INVALID_CAP_ID;

    bool allocated = false;
    std::uint32_t target_slot = start_slot;
    if (target_slot == 0) {
        target_slot = g_slot_allocator.allocate_contiguous(count);
        if (target_slot == INVALID_CAP_ID) return INVALID_CAP_ID;
        allocated = true;
    }

    int res = sys_untyped_retype_bulk(
        untyped_cap,
        ObjectType::Frame,
        0,
        0,          // 自 CSpace
        target_slot,
        count
    );
    if (res != 0) {
        if (allocated) g_slot_allocator.free(target_slot, count);
        return INVALID_CAP_ID;
    }
    return target_slot;
}

inline std::uint32_t sys_frame_alloc(std::uint32_t untyped_cap, std::uint64_t num_pages) {
    return sys_frame_alloc(untyped_cap, 0, num_pages);
}

inline std::uint32_t sys_cap_mint(std::uint32_t src_cap, std::uint32_t new_rights, std::uint64_t badge = 0) {
    std::uint32_t slot = g_slot_allocator.allocate_one();
    if (slot == INVALID_CAP_ID) return INVALID_CAP_ID;

    int res = sys_cap_mint(0, slot, src_cap, new_rights, badge);
    if (res != 0) {
        g_slot_allocator.free(slot);
        return INVALID_CAP_ID;
    }
    return slot;
}

inline std::uint32_t sys_pagetable_create(std::uint32_t untyped_cap, std::uint32_t frame_cap, std::uint32_t level) {
    std::uint32_t slot = g_slot_allocator.allocate_one();
    if (slot == INVALID_CAP_ID) return INVALID_CAP_ID;

    int res = sys_pagetable_create(untyped_cap, frame_cap, level, 0, slot);
    if (res != 0) {
        g_slot_allocator.free(slot);
        return INVALID_CAP_ID;
    }
    return slot;
}

inline std::uint32_t sys_baton_create_irq(std::uint32_t irq_control_cap, std::uint32_t untyped_cap, std::uint64_t initial_budget_us) {
    std::uint32_t slot = g_slot_allocator.allocate_one();
    if (slot == INVALID_CAP_ID) return INVALID_CAP_ID;

    int res = sys_baton_create_irq(irq_control_cap, untyped_cap, initial_budget_us, 0, slot);
    if (res != 0) {
        g_slot_allocator.free(slot);
        return INVALID_CAP_ID;
    }
    return slot;
}

inline std::uint32_t sys_irq_handler_create(std::uint32_t untyped_cap, std::uint8_t vector, std::uint32_t notification_ep_cap) {
    std::uint32_t slot = g_slot_allocator.allocate_one();
    if (slot == INVALID_CAP_ID) return INVALID_CAP_ID;

    int res = sys_irq_handler_create(untyped_cap, vector, notification_ep_cap, 0, slot);
    if (res != 0) {
        g_slot_allocator.free(slot);
        return INVALID_CAP_ID;
    }
    return slot;
}

inline std::uint32_t sys_ioport_create(std::uint32_t parent_ioport_cap, std::uint32_t untyped_cap, std::uint16_t base_port, std::uint32_t port_count) {
    (void)untyped_cap;
    std::uint32_t slot = g_slot_allocator.allocate_one();
    if (slot == INVALID_CAP_ID) return INVALID_CAP_ID;

    int res = sys_ioport_create(parent_ioport_cap, base_port, port_count, 0, slot);
    if (res != 0) {
        g_slot_allocator.free(slot);
        return INVALID_CAP_ID;
    }
    return slot;
}

inline int sys_cspace_create(std::uint32_t untyped_cap, std::uint32_t target_cspace, std::uint32_t target_slot) {
    return sys_untyped_retype_bulk(untyped_cap, ObjectType::CSpaceCap, 0, target_cspace, target_slot, 1);
}

inline std::uint32_t sys_cspace_create(std::uint32_t untyped_cap) {
    std::uint32_t slot = g_slot_allocator.allocate_one();
    if (slot == INVALID_CAP_ID) return INVALID_CAP_ID;

    int res = sys_cspace_create(untyped_cap, 0, slot);
    if (res != 0) {
        g_slot_allocator.free(slot);
        return INVALID_CAP_ID;
    }
    return slot;
}

inline int sys_notification_create(std::uint32_t untyped_cap, std::uint32_t target_cspace, std::uint32_t target_slot) {
    return sys_untyped_retype_bulk(untyped_cap, ObjectType::Notification, 0, target_cspace, target_slot, 1);
}

inline std::uint32_t sys_notification_create(std::uint32_t untyped_cap) {
    std::uint32_t slot = g_slot_allocator.allocate_one();
    if (slot == INVALID_CAP_ID) return INVALID_CAP_ID;

    int res = sys_notification_create(untyped_cap, 0, slot);
    if (res != 0) {
        g_slot_allocator.free(slot);
        return INVALID_CAP_ID;
    }
    return slot;
}

// Well-known capability slot constants
static constexpr std::uint32_t CAP_SELF_VSPACE = 0;
static constexpr std::uint32_t CAP_UNTYPED = 240;
static constexpr std::uint32_t CAP_AHCI_MMIO = 241;
static constexpr std::uint32_t CAP_XHCI_MMIO = 242;
static constexpr std::uint32_t CAP_BOOTINFO = 243;
static constexpr std::uint32_t CAP_INITRD = 244;
static constexpr std::uint32_t CAP_SELF_CSPACE = 245;
static constexpr std::uint32_t CAP_FRAMEBUFFER = 246;
static constexpr std::uint32_t CAP_ROOT_IOPORT = 247;
static constexpr std::uint32_t CAP_ROOT_BATON = 248;
static constexpr std::uint32_t CAP_IRQ_CONTROL = 249;
static constexpr std::uint32_t CAP_NULL = 0;

// ============================================================================
// C++ RAII helpers for Linear & Once Capabilities
// ============================================================================

template <typename Tag = void>
class LinearCap {
private:
    std::uint32_t slot_{INVALID_CAP_ID};

public:
    LinearCap() = default;
    explicit LinearCap(std::uint32_t slot) : slot_(slot) {}

    ~LinearCap() {
        reset();
    }

    LinearCap(const LinearCap&) = delete;
    LinearCap& operator=(const LinearCap&) = delete;

    LinearCap(LinearCap&& other) noexcept : slot_(other.slot_) {
        other.slot_ = INVALID_CAP_ID;
    }

    LinearCap& operator=(LinearCap&& other) noexcept {
        if (this != &other) {
            reset();
            slot_ = other.slot_;
            other.slot_ = INVALID_CAP_ID;
        }
        return *this;
    }

    bool isValid() const { return slot_ != INVALID_CAP_ID; }
    std::uint32_t get() const { return slot_; }

    std::uint32_t release() {
        std::uint32_t s = slot_;
        slot_ = INVALID_CAP_ID;
        return s;
    }

    void reset(std::uint32_t new_slot = INVALID_CAP_ID) {
        if (slot_ != INVALID_CAP_ID && slot_ != 0) {
            sys_cap_delete(slot_);
        }
        slot_ = new_slot;
    }

    // Move capability to another slot
    bool moveTo(std::uint32_t dest_slot) {
        if (!isValid()) return false;
        if (sys_cap_move(dest_slot, slot_) == 0) {
            g_slot_allocator.free(slot_);
            slot_ = dest_slot;
            return true;
        }
        return false;
    }
};

template <typename Tag = void>
using ScopedCap = LinearCap<Tag>;



} // namespace oz
