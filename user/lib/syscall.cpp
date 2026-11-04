#include "syscall.hpp"

namespace oz {

constinit CSpaceSlotAllocator g_slot_allocator;

extern "C" {

std::uint64_t syscall(std::uint64_t number, std::uint64_t arg1, std::uint64_t arg2, std::uint64_t arg3, std::uint64_t arg4, std::uint64_t arg5, std::uint64_t arg6) {
    std::uint64_t ret;
    register std::uint64_t r10 __asm__("r10") = arg4;
    register std::uint64_t r8  __asm__("r8")  = arg5;
    register std::uint64_t r9  __asm__("r9")  = arg6;
    
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void sys_exit(int status) {
    syscall(60, static_cast<std::uint64_t>(status), 0, 0, 0, 0, 0);
    while (1) {
        __asm__ volatile("pause");
    }
}

int sys_yield() {
    return static_cast<int>(syscall(44, 0, 0, 0, 0, 0, 0));
}

int sys_untyped_retype_bulk(std::uint32_t untyped_cap, ObjectType target_type, std::uint64_t size_or_arg, std::uint32_t target_cspace, std::uint32_t start_slot, std::size_t count) {
    return static_cast<int>(syscall(59, untyped_cap, static_cast<std::uint64_t>(target_type), size_or_arg, target_cspace, start_slot, count));
}

int sys_channel_create(std::uint32_t untyped_cap,
                       std::uint32_t target_cs0, std::uint32_t slot0,
                       std::uint32_t target_cs1, std::uint32_t slot1,
                       ChannelMode mode0, ChannelMode mode1) {
    std::uint64_t modes = static_cast<std::uint32_t>(mode0) | (static_cast<std::uint64_t>(mode1) << 32);
    return static_cast<int>(syscall(3, untyped_cap, target_cs0, slot0, target_cs1, slot1, modes));
}

bool sys_call(std::uint32_t cap_id, const IPCMessage* send_msg, IPCMessage* recv_msg, std::uint32_t recv_slot) {
    return sys_call_ext(cap_id, send_msg, recv_msg, 0, recv_slot) == 0;
}

bool sys_call_recv_cap(std::uint32_t cap_id, const IPCMessage* send_msg, IPCMessage* recv_msg, std::uint32_t* out_slot) {
    std::uint32_t slot = g_slot_allocator.allocate_one();
    if (slot == INVALID_CAP_ID) return false;

    int ret = sys_call_ext(cap_id, send_msg, recv_msg, 0, slot);
    if (ret == 0) {
        if (recv_msg && recv_msg->has_cap) {
            recv_msg->transferred_cap = slot;
            if (out_slot) *out_slot = slot;
        } else {
            g_slot_allocator.free(slot);
            if (recv_msg) recv_msg->transferred_cap = 0;
            if (out_slot) *out_slot = 0;
        }
        return true;
    }

    g_slot_allocator.free(slot);
    if (recv_msg) recv_msg->transferred_cap = 0;
    if (out_slot) *out_slot = 0;
    return false;
}

int sys_call_ext(std::uint32_t cap_id, const IPCMessage* send_msg, IPCMessage* recv_msg, std::uint32_t flags, std::uint32_t recv_slot) {
    std::uint32_t effective_slot = (recv_slot == IPC_RECV_SLOT_NONE) ? 0 : recv_slot;

    return static_cast<int>(syscall(27, cap_id,
                                    reinterpret_cast<std::uint64_t>(send_msg),
                                    reinterpret_cast<std::uint64_t>(recv_msg),
                                    flags,
                                    effective_slot,
                                    0));
}

int sys_reply_and_recv(std::uint32_t ep_cap, const IPCMessage* reply_msg, IPCMessage* recv_msg, std::uint32_t flags, std::uint32_t recv_slot) {
    std::uint32_t effective_slot = (recv_slot == IPC_RECV_SLOT_NONE) ? 0 : recv_slot;

    return static_cast<int>(syscall(28, ep_cap,
                                    reinterpret_cast<std::uint64_t>(reply_msg),
                                    reinterpret_cast<std::uint64_t>(recv_msg),
                                    flags,
                                    effective_slot,
                                    0));
}

ReplyToken sys_reply_recv(ReplyToken reply_token, std::uint32_t ep_cap, const IPCMessage* reply_msg, IPCMessage* recv_msg, std::uint32_t recv_slot) {
    (void)reply_token;
    int res = sys_reply_and_recv(ep_cap, reply_msg, recv_msg, IPC_FLAG_BLOCK, recv_slot);
    if (res == 0) return 1;
    return INVALID_REPLY_TOKEN;
}

std::size_t sys_write(const char* str) {
    if (!str) return 0;
    sys_debug_print(str);
    return 0;
}


PtCacheEntry g_pt_cache[PT_CACHE_SIZE];

static inline std::size_t pt_cache_hash(std::uint32_t vspace_cap, std::uint8_t level, std::uint64_t va_prefix) {
    std::uint64_t h = (static_cast<std::uint64_t>(vspace_cap) << 32) ^ (static_cast<std::uint64_t>(level) << 24) ^ va_prefix;
    h ^= (h >> 33);
    h *= 0xff51afd7ed558ccdULL;
    h ^= (h >> 33);
    return static_cast<std::size_t>(h & (PT_CACHE_SIZE - 1));
}

static std::uint32_t cache_lookup(std::uint32_t vspace_cap, std::uint8_t level, std::uint64_t va_prefix) {
    std::size_t start_idx = pt_cache_hash(vspace_cap, level, va_prefix);
    for (std::size_t step = 0; step < PT_CACHE_SIZE; ++step) {
        std::size_t i = (start_idx + step) & (PT_CACHE_SIZE - 1);
        if (!g_pt_cache[i].valid) {
            return 0; // Empty slot: not found
        }
        if (g_pt_cache[i].vspace_cap == vspace_cap &&
            g_pt_cache[i].level == level &&
            g_pt_cache[i].va_prefix == va_prefix) {
            return g_pt_cache[i].pt_cap;
        }
    }
    return 0;
}

static void cache_insert(std::uint32_t vspace_cap, std::uint8_t level, std::uint64_t va_prefix, std::uint32_t pt_cap) {
    std::size_t start_idx = pt_cache_hash(vspace_cap, level, va_prefix);
    for (std::size_t step = 0; step < PT_CACHE_SIZE; ++step) {
        std::size_t i = (start_idx + step) & (PT_CACHE_SIZE - 1);
        if (!g_pt_cache[i].valid) {
            g_pt_cache[i].vspace_cap = vspace_cap;
            g_pt_cache[i].level = level;
            g_pt_cache[i].va_prefix = va_prefix;
            g_pt_cache[i].pt_cap = pt_cap;
            g_pt_cache[i].valid = true;
            return;
        }
        if (g_pt_cache[i].vspace_cap == vspace_cap &&
            g_pt_cache[i].level == level &&
            g_pt_cache[i].va_prefix == va_prefix) {
            g_pt_cache[i].pt_cap = pt_cap;
            return;
        }
    }
    // Cache is full: never evict existing entries to avoid permanent SYS_ERR_BUSY map failures
}

int sys_pagetable_create(std::uint32_t untyped_cap, std::uint32_t frame_cap, std::uint32_t level, std::uint32_t target_cspace, std::uint32_t target_slot) {
    return static_cast<int>(syscall(17, untyped_cap, frame_cap, level, target_cspace, target_slot, 0));
}

int sys_pagetable_map(std::uint32_t parent_pt_cap, std::uint32_t index, std::uint32_t child_pt_cap) {
    return static_cast<int>(syscall(25, parent_pt_cap, index, child_pt_cap, 0, 0, 0));
}

int sys_pagetable_map_frame(std::uint32_t pt_cap, std::uint32_t index, std::uint32_t frame_cap, std::uint64_t flags) {
    return static_cast<int>(syscall(4, pt_cap, index, frame_cap, flags, 0, 0));
}

int sys_cap_unmap(std::uint32_t mapped_cap) {
    return static_cast<int>(syscall(7, mapped_cap, 0, 0, 0, 0, 0));
}

int sys_cap_identify(std::uint32_t cap_id, CapInfo* info) {
    return static_cast<int>(syscall(58, cap_id, reinterpret_cast<std::uint64_t>(info), 0, 0, 0, 0));
}

std::uint32_t sys_pagetable_alloc(std::uint32_t untyped_cap, std::uint32_t level) {
    std::uint32_t frame_cap = sys_untyped_retype(untyped_cap, static_cast<std::uint32_t>(ObjectType::Frame), 4096);
    if (frame_cap == static_cast<std::uint32_t>(-1)) return static_cast<std::uint32_t>(-1);
    std::uint32_t pt_cap = sys_pagetable_create(untyped_cap, frame_cap, level);
    if (pt_cap == static_cast<std::uint32_t>(-1)) {
        sys_cap_delete(frame_cap);
        return static_cast<std::uint32_t>(-1);
    }
    return pt_cap;
}

std::uint32_t sys_alloc_shared_pt(std::uint32_t untyped_cap, std::size_t num_pages, std::uint64_t flags) {
    if (num_pages == 0 || num_pages > 512) return static_cast<std::uint32_t>(-1);
    std::uint32_t pt_cap = sys_pagetable_alloc(untyped_cap, 1);
    if (pt_cap == static_cast<std::uint32_t>(-1)) return static_cast<std::uint32_t>(-1);

    std::uint32_t start_frame = sys_frame_alloc(untyped_cap, 0, num_pages);
    if (start_frame == static_cast<std::uint32_t>(-1)) {
        sys_cap_delete(pt_cap);
        return static_cast<std::uint32_t>(-1);
    }

    for (std::size_t i = 0; i < num_pages; ++i) {
        int res = sys_pagetable_map_frame(pt_cap, static_cast<std::uint32_t>(i), start_frame + i, flags);
        if (res != 0) {
            for (std::size_t j = 0; j < i; ++j) {
                sys_cap_unmap(start_frame + j);
            }
            for (std::size_t j = 0; j < num_pages; ++j) {
                sys_cap_delete(start_frame + j);
            }
            sys_cap_delete(pt_cap);
            return static_cast<std::uint32_t>(-1);
        }
    }
    return pt_cap;
}

std::uint32_t sys_ensure_intermediate_pt(std::uint32_t vspace_cap, std::uint32_t untyped_cap, std::uint64_t virt_addr, std::uint32_t target_level) {
    if (target_level < 1 || target_level > 3) return static_cast<std::uint32_t>(-1);

    std::uint64_t pml4_idx = (virt_addr >> 39) & 0x1FFULL;
    std::uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FFULL;
    std::uint64_t pd_idx   = (virt_addr >> 21) & 0x1FFULL;

    // 1. Level 3 (PDPT)
    std::uint32_t pdpt_cap = cache_lookup(vspace_cap, 3, pml4_idx);
    if (pdpt_cap == 0) {
        pdpt_cap = sys_pagetable_alloc(untyped_cap, 3);
        if (pdpt_cap == static_cast<std::uint32_t>(-1)) return static_cast<std::uint32_t>(-1);
        int res = sys_pagetable_map(vspace_cap, static_cast<std::uint32_t>(pml4_idx), pdpt_cap);
        if (res != 0) {
            sys_cap_delete(pdpt_cap);
            return static_cast<std::uint32_t>(-1);
        }
        cache_insert(vspace_cap, 3, pml4_idx, pdpt_cap);
    }
    if (target_level == 3) return pdpt_cap;

    // 2. Level 2 (PD)
    std::uint64_t prefix_pd = (pml4_idx << 9) | pdpt_idx;
    std::uint32_t pd_cap = cache_lookup(vspace_cap, 2, prefix_pd);
    if (pd_cap == 0) {
        pd_cap = sys_pagetable_alloc(untyped_cap, 2);
        if (pd_cap == static_cast<std::uint32_t>(-1)) return static_cast<std::uint32_t>(-1);
        int res = sys_pagetable_map(pdpt_cap, static_cast<std::uint32_t>(pdpt_idx), pd_cap);
        if (res != 0) {
            sys_cap_delete(pd_cap);
            return static_cast<std::uint32_t>(-1);
        }
        cache_insert(vspace_cap, 2, prefix_pd, pd_cap);
    }
    if (target_level == 2) return pd_cap;

    // 3. Level 1 (PT)
    std::uint64_t prefix_pt = (prefix_pd << 9) | pd_idx;
    std::uint32_t pt_cap = cache_lookup(vspace_cap, 1, prefix_pt);
    if (pt_cap == 0) {
        pt_cap = sys_pagetable_alloc(untyped_cap, 1);
        if (pt_cap == static_cast<std::uint32_t>(-1)) return static_cast<std::uint32_t>(-1);
        int res = sys_pagetable_map(pd_cap, static_cast<std::uint32_t>(pd_idx), pt_cap);
        if (res != 0) {
            sys_cap_delete(pt_cap);
            return static_cast<std::uint32_t>(-1);
        }
        cache_insert(vspace_cap, 1, prefix_pt, pt_cap);
    }
    return pt_cap;
}

int sys_vspace_map_pagetable(std::uint32_t vspace_cap, std::uint32_t pagetable_cap,
                             std::uint64_t virt_addr, std::uint32_t level) {
    (void)vspace_cap; (void)pagetable_cap; (void)virt_addr; (void)level;
    return -1;
}

int sys_vspace_ensure_pagetables_range(std::uint32_t vspace_cap, std::uint32_t untyped_cap,
                                       std::uint64_t start_vaddr, std::uint64_t size) {
    std::uint64_t end_vaddr = start_vaddr + size;
    std::uint64_t cur = start_vaddr & ~0xFFFULL;
    while (cur < end_vaddr) {
        if (sys_ensure_intermediate_pt(vspace_cap, untyped_cap, cur, 1) == static_cast<std::uint32_t>(-1)) {
            return -1;
        }
        cur += 0x200000ULL;
    }
    return 0;
}

int sys_vspace_map_with_pagetables(std::uint32_t vspace_cap, std::uint32_t untyped_cap,
                                   std::uint32_t cap, std::uint64_t virt_addr,
                                   std::uint64_t flags, std::uint64_t size) {
    CapInfo info{};
    int id_res = sys_cap_identify(cap, &info);
    if (id_res != 0) {
        info.type = CAP_TYPE_FRAME; // Fallback: Frame
    }

    if (info.type == CAP_TYPE_FRAME) {
        std::uint64_t num_pages = (size > 0) ? ((size + 4095) / 4096) : 1;
        for (std::uint64_t p = 0; p < num_pages; ++p) {
            std::uint64_t cur_virt = virt_addr + p * 4096;
            std::uint32_t cur_cap = cap + static_cast<std::uint32_t>(p);
            std::uint32_t pt_cap = sys_ensure_intermediate_pt(vspace_cap, untyped_cap, cur_virt, 1);
            if (pt_cap == static_cast<std::uint32_t>(-1)) {
                for (std::uint64_t r = 0; r < p; ++r) {
                    sys_cap_unmap(cap + static_cast<std::uint32_t>(r));
                }
                return -1;
            }
            std::uint16_t pt_idx = static_cast<std::uint16_t>((cur_virt >> 12) & 0x1FFULL);
            int res = sys_pagetable_map_frame(pt_cap, pt_idx, cur_cap, flags);
            if (res != 0) {
                for (std::uint64_t r = 0; r < p; ++r) {
                    sys_cap_unmap(cap + static_cast<std::uint32_t>(r));
                }
                return res;
            }
        }
        return 0;
    } else if (info.type == CAP_TYPE_PAGETABLE) {
        if (info.level == 1) {
            std::uint32_t pd_cap = sys_ensure_intermediate_pt(vspace_cap, untyped_cap, virt_addr, 2);
            if (pd_cap == static_cast<std::uint32_t>(-1)) return -1;
            std::uint16_t pd_idx = static_cast<std::uint16_t>((virt_addr >> 21) & 0x1FFULL);
            return sys_pagetable_map(pd_cap, pd_idx, cap);
        } else if (info.level == 2) {
            std::uint32_t pdpt_cap = sys_ensure_intermediate_pt(vspace_cap, untyped_cap, virt_addr, 3);
            if (pdpt_cap == static_cast<std::uint32_t>(-1)) return -1;
            std::uint16_t pdpt_idx = static_cast<std::uint16_t>((virt_addr >> 30) & 0x1FFULL);
            return sys_pagetable_map(pdpt_cap, pdpt_idx, cap);
        }
    }
    return -1;
}

int sys_vspace_map(std::uint32_t vspace_cap, std::uint32_t frame_cap,
                   std::uint64_t virt_addr, std::uint64_t flags) {
    return sys_vspace_map_with_pagetables(vspace_cap, 240, frame_cap, virt_addr, flags, 4096);
}

int sys_vspace_unmap(std::uint32_t vspace_cap, std::uint64_t virt_addr_or_cap) {
    (void)vspace_cap;
    // Note: Since virtual address unmapping is deprecated, unmap is performed via Capability ID.
    if (virt_addr_or_cap < 4096) {
        return sys_cap_unmap(static_cast<std::uint32_t>(virt_addr_or_cap));
    }
    return -1;
}

std::uint64_t sys_frame_phys_addr(std::uint32_t frame_cap) {
    return syscall(8, frame_cap, 0, 0, 0, 0, 0);
}

std::uint64_t sys_frame_size(std::uint32_t frame_cap) {
    return syscall(26, frame_cap, 0, 0, 0, 0, 0);
}

void sys_debug_print(const char* str) {
    syscall(6, reinterpret_cast<std::uint64_t>(str), 0, 0, 0, 0, 0);
}

int sys_cap_mint(std::uint32_t target_cspace, std::uint32_t target_slot, std::uint32_t src_cap, std::uint32_t new_rights, std::uint64_t badge) {
    return static_cast<int>(syscall(9, target_cspace, target_slot, src_cap, new_rights, badge, 0));
}

int sys_cap_revoke(std::uint32_t cap_id) {
    return static_cast<int>(syscall(55, cap_id, 0, 0, 0, 0, 0));
}

int sys_cap_delete(std::uint32_t cap_id) {
    int res = static_cast<int>(syscall(10, cap_id, 0, 0, 0, 0, 0));
    if (res == 0) {
        g_slot_allocator.free(cap_id);
    }
    return res;
}

int sys_tcb_configure(std::uint32_t tcb_cap, std::uint32_t cspace_cap,
                      std::uint32_t vspace_cap, std::uint64_t rip,
                      std::uint64_t user_rsp) {
    return static_cast<int>(syscall(11, tcb_cap, cspace_cap, vspace_cap, rip, user_rsp, 0));
}

int sys_tcb_resume(std::uint32_t tcb_cap) {
    return static_cast<int>(syscall(13, tcb_cap, 0, 0, 0, 0, 0));
}

int sys_tcb_suspend(std::uint32_t tcb_cap) {
    return static_cast<int>(syscall(14, tcb_cap, 0, 0, 0, 0, 0));
}

int sys_tcb_set_fault_endpoint(std::uint32_t tcb_cap, std::uint32_t ep_cap) {
    return static_cast<int>(syscall(29, tcb_cap, ep_cap, 0, 0, 0, 0));
}

int sys_tcb_set_exit_notification(std::uint32_t tcb_cap, std::uint32_t notif_cap, std::uint64_t badge) {
    return static_cast<int>(syscall(39, tcb_cap, notif_cap, badge, 0, 0, 0));
}

int sys_cspace_cap_insert(std::uint32_t cspace_cap, std::uint32_t target_slot,
                          std::uint32_t src_cap, std::uint32_t rights) {
    return static_cast<int>(syscall(16, cspace_cap, target_slot, src_cap, rights, 0, 0));
}

std::uint32_t sys_vspace_create(std::uint32_t untyped_cap) {
    return sys_pagetable_alloc(untyped_cap, 4);
}

int sys_irq_handler_create(std::uint32_t untyped_cap, std::uint8_t vector, std::uint32_t notification_ep_cap, std::uint32_t target_cspace, std::uint32_t target_slot) {
    return static_cast<int>(syscall(18, untyped_cap, vector, notification_ep_cap, target_cspace, target_slot, 0));
}

int sys_irq_ack(std::uint32_t irq_handler_cap) {
    return static_cast<int>(syscall(47, irq_handler_cap, 0, 0, 0, 0, 0));
}

int sys_ioport_create(std::uint32_t parent_ioport_cap, std::uint16_t base_port, std::uint32_t port_count, std::uint32_t target_cspace, std::uint32_t target_slot) {
    return static_cast<int>(syscall(20, parent_ioport_cap, base_port, port_count, target_cspace, target_slot, 0));
}

std::uint64_t sys_ioport_read(std::uint32_t ioport_cap, std::uint16_t port, std::uint32_t width) {
    return syscall(21, ioport_cap, port, width, 0, 0, 0);
}

int sys_ioport_write(std::uint32_t ioport_cap, std::uint16_t port, std::uint64_t value, std::uint32_t width) {
    return static_cast<int>(syscall(22, ioport_cap, port, value, width, 0, 0));
}

int sys_untyped_revoke(std::uint32_t untyped_cap) {
    return static_cast<int>(syscall(24, untyped_cap, 0, 0, 0, 0, 0));
}

int sys_cap_move(std::uint32_t dest_slot, std::uint32_t src_slot) {
    return static_cast<int>(syscall(30, dest_slot, src_slot, 0, 0, 0, 0));
}

int sys_cspace_cap_move(std::uint32_t target_cspace_cap, std::uint32_t target_slot, std::uint32_t src_slot) {
    return static_cast<int>(syscall(31, target_cspace_cap, target_slot, src_slot, 0, 0, 0));
}

int sys_cap_copy(std::uint32_t dest_slot, std::uint32_t src_slot, std::uint32_t rights) {
    return static_cast<int>(syscall(32, dest_slot, src_slot, rights, 0, 0, 0));
}

int sys_notification_signal(std::uint32_t notif_cap, std::uint64_t badge) {
    return static_cast<int>(syscall(35, notif_cap, badge, 0, 0, 0, 0));
}

int sys_notification_wait(std::uint32_t notif_cap, std::uint64_t* out_badge) {
    return static_cast<int>(syscall(36, notif_cap, reinterpret_cast<std::uint64_t>(out_badge), 0, 0, 0, 0));
}

int sys_channel_bind_notification(std::uint32_t ep_cap, std::uint32_t notif_cap, std::uint64_t badge) {
    return static_cast<int>(syscall(37, ep_cap, notif_cap, badge, 0, 0, 0));
}

int sys_notification_poll(std::uint32_t notif_cap, std::uint64_t* out_badge) {
    return static_cast<int>(syscall(38, notif_cap, reinterpret_cast<std::uint64_t>(out_badge), 0, 0, 0, 0));
}

int sys_notification_bind_parent(std::uint32_t child_notif_cap, std::uint32_t parent_notif_cap, std::uint64_t badge) {
    return static_cast<int>(syscall(51, child_notif_cap, parent_notif_cap, badge, 0, 0, 0));
}

int sys_notification_unbind_parent(std::uint32_t child_notif_cap) {
    return static_cast<int>(syscall(52, child_notif_cap, 0, 0, 0, 0, 0));
}

int sys_tcb_cancel_ipc(std::uint32_t tcb_cap) {
    return static_cast<int>(syscall(40, tcb_cap, 0, 0, 0, 0, 0));
}

int sys_tcb_get_cpu_time(std::uint32_t tcb_cap, std::uint64_t* out_cycles) {
    return static_cast<int>(syscall(41, tcb_cap, reinterpret_cast<std::uint64_t>(out_cycles), 0, 0, 0, 0));
}

int sys_tcb_set_sched_notification(std::uint32_t tcb_cap, std::uint32_t notif_cap, std::uint64_t badge) {
    return static_cast<int>(syscall(46, tcb_cap, notif_cap, badge, 0, 0, 0));
}

int sys_tcb_bind_baton(std::uint32_t tcb_cap, std::uint32_t baton_cap) {
    return static_cast<int>(syscall(53, tcb_cap, baton_cap, 0, 0, 0, 0));
}

int sys_baton_bind_notification(std::uint32_t baton_cap, std::uint32_t notif_cap, std::uint64_t badge) {
    return static_cast<int>(syscall(54, baton_cap, notif_cap, badge, 0, 0, 0));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type-c-linkage"
SchedResult sys_baton_switch(std::uint32_t target_baton_cap, std::uint64_t quantum_us) {
    register std::uint64_t rax __asm__("rax") = 42;
    register std::uint64_t rdi __asm__("rdi") = target_baton_cap;
    register std::uint64_t rsi __asm__("rsi") = quantum_us;
    register std::uint64_t rdx __asm__("rdx") = 0;
    register std::uint64_t r10 __asm__("r10") = 0;
    register std::uint64_t r8  __asm__("r8")  = 0;
    register std::uint64_t r9  __asm__("r9")  = 0;

    __asm__ volatile(
        "syscall"
        : "+a"(rax), "+d"(rdx)
        : "r"(rdi), "r"(rsi), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );

    SchedResult res;
    res.reason = static_cast<SchedReason>(rax);
    res.consumed_cycles = rdx;
    return res;
}

SchedResult sys_baton_split(std::uint32_t target_tcb_cap, std::uint64_t budget_us) {
    (void)target_tcb_cap; (void)budget_us;
    return SchedResult{};
}

SchedResult sys_donate_time(std::uint32_t target_tcb_cap, std::uint64_t time_budget_us) {
    (void)target_tcb_cap; (void)time_budget_us;
    return SchedResult{};
}
#pragma GCC diagnostic pop

int sys_baton_inject(std::uint32_t target_baton_cap, std::uint64_t budget_us) {
    return static_cast<int>(syscall(43, target_baton_cap, budget_us, 0, 0, 0, 0));
}

int sys_baton_set_budget(std::uint32_t baton_cap, std::uint64_t budget_us) {
    return static_cast<int>(syscall(57, baton_cap, budget_us, 0, 0, 0, 0));
}

int sys_baton_create_irq(std::uint32_t irq_control_cap, std::uint32_t untyped_cap, std::uint64_t initial_budget_us, std::uint32_t target_cspace, std::uint32_t target_slot) {
    return static_cast<int>(syscall(56, irq_control_cap, untyped_cap, initial_budget_us, target_cspace, target_slot, 0));
}

int sys_thread_set_notification(std::uint32_t thread_cap, std::uint32_t notif_cap, std::uint64_t badge) {
    return static_cast<int>(syscall(46, thread_cap, notif_cap, badge, 0, 0, 0));
}

int sys_irq_wait(std::uint32_t irq_handler_cap, std::uint32_t irq_baton_cap) {
    return static_cast<int>(syscall(50, irq_handler_cap, irq_baton_cap, 0, 0, 0, 0));
}

int sys_irq_handler_set_notification(std::uint32_t irq_handler_cap, std::uint32_t notif_cap, std::uint64_t badge) {
    return static_cast<int>(syscall(48, irq_handler_cap, notif_cap, badge, 0, 0, 0));
}

int sys_tcb_get_id(std::uint32_t tcb_cap, std::uint64_t* out_id) {
    return static_cast<int>(syscall(45, tcb_cap, reinterpret_cast<std::uint64_t>(out_id), 0, 0, 0, 0));
}

} // extern "C"

} // namespace oz
