#pragma once
#include <oz/SyscallNumbers.hpp>
#include <oz/ErrorCodes.hpp>
#include <oz/CapabilityTypes.hpp>
#include <oz/IPCProtocol.hpp>
#include <oz/SchedTypes.hpp>
#include "cspace_allocator.hpp"
#include <cstdint>
#include <cstddef>

namespace oz {

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

inline std::uint64_t syscall(Syscall number,
                             std::uint64_t arg1 = 0, std::uint64_t arg2 = 0,
                             std::uint64_t arg3 = 0, std::uint64_t arg4 = 0,
                             std::uint64_t arg5 = 0, std::uint64_t arg6 = 0) {
    return syscall(static_cast<std::uint64_t>(number), arg1, arg2, arg3, arg4, arg5, arg6);
}

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
