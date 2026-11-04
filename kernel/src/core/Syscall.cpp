#include "core/Syscall.hpp"
#include "hardware/x86_64.hpp"
#include "core/CapOperations.hpp"
#include "core/Thread.hpp"
#include "utils/utils.hpp"
#include "core/VSpace.hpp"
#include "core/UntypedMemory.hpp"
#include "core/TCB.hpp"
#include "core/CSpaceCap.hpp"
#include "core/IRQHandler.hpp"
#include "core/IRQDispatch.hpp"
#include "core/Notification.hpp"
#include "core/Baton.hpp"
#include "core/CartesianTree.hpp"

#include "core/Kernel.hpp"
#include "core/SavedInterruptFrame.hpp"

namespace oz {
constexpr int SYS_ERR_INVALID_STATE = -7;
constexpr int SYS_ERR_PERM          = -8;
constexpr int SYS_ERR_NO_MEMORY     = -9;

void (*g_exit_thread_func)() = nullptr;
CSpace* (*g_get_current_cspace_func)() = nullptr;
Thread* (*g_get_current_thread_func)() = nullptr;
ExecutionContext* (*g_get_current_context_func)() = nullptr;
void (*g_block_thread_func)() = nullptr;
void (*g_unblock_thread_func)(Thread*) = nullptr;
void (*g_queue_thread_func)(Thread* t) = nullptr;

SyscallContext g_syscall_context;

extern "C" void syscall_entry();



// ===========================================================================
// Phase 5: Security helpers
// ===========================================================================

// User-space pointer validation
// x86-64 canonical lower-half: addresses < 0x0000800000000000 are user-space
static constexpr std::uintptr_t USER_SPACE_LIMIT = 0x0000800000000000ULL;

static bool is_user_address(std::uintptr_t addr) {
    return addr != 0 && addr < USER_SPACE_LIMIT;
}

static bool is_user_buffer(std::uintptr_t addr, std::size_t size) {
    if (addr == 0 || size == 0) return false;
    if (addr >= USER_SPACE_LIMIT) return false;
    if (addr + size < addr) return false;  // overflow check
    if (addr + size > USER_SPACE_LIMIT) return false;
    return true;
}

// Type-safe capability extraction: returns typed pointer or nullptr
template <typename T>
static T* get_cap_typed(CSpace* cspace, CapID id, ObjectType expected, CapRights required) {
    if (!cspace) return nullptr;
    Capability* slot = cspace->getSlot(id);
    if (!slot || slot->is_null()) return nullptr;
    if (!slot->has_rights(required)) return nullptr;
    if (slot->type() != to_cap_type(expected)) return nullptr;
    return static_cast<T*>(slot->object());
}

// Capability システムにおけるポリモーフィックな型解決ヘルパー
static inline ExecutionContext* get_cap_execution_context(CSpace* cspace, CapID cap_id, CapRights rights) {
    if (!cspace) return nullptr;
    Capability* slot = cspace->getSlot(cap_id);
    if (!slot || slot->is_null() || !slot->object()) return nullptr;
    if (!slot->has_rights(rights)) return nullptr;
    if (slot->type() == CapType::TCB) {
        TCB* tcb = static_cast<TCB*>(slot->object());
        return tcb->getThread();
    }
    return nullptr;
}

inline void handleSelfDestructionIfNeeded(Thread* current) {
    if (!current) return;
    std::uint64_t now = oz::x86_64::rdtsc();
    Scheduler& sched = Kernel::instance().getScheduler();

    // ケース ① current->is_dead == true（自身の TCB が破棄された場合）
    if (current->is_dead || current->state == ThreadState::Dead) {
        ExecutionContext* next_ctx = nullptr;
        if (current->saved_frame.interrupted_context &&
            current->saved_frame.interrupted_context != current &&
            !current->saved_frame.interrupted_context->is_dead) {
            next_ctx = current->saved_frame.interrupted_context;
        } else if (g_current_irq_thread && g_current_irq_thread != current && !g_current_irq_thread->is_dead) {
            next_ctx = g_current_irq_thread;
        } else if (sched.running_baton && sched.running_baton->thread &&
                   sched.running_baton->thread != current && !sched.running_baton->thread->is_dead) {
            next_ctx = sched.running_baton->thread;
        } else {
            next_ctx = sched.main_thread;
        }
        sched.abandonCurrentContextTo(next_ctx, now);
    }

    // ケース ② !current->has_baton()（実行中の Baton が破棄され、TCB は生存している場合）
    if (!current->get_active_baton()) {
        current->state = ThreadState::Ready;
        ExecutionContext* next_ctx = nullptr;
        if (sched.running_baton && sched.running_baton->thread &&
            sched.running_baton->thread != current && !sched.running_baton->thread->is_dead) {
            next_ctx = sched.running_baton->thread;
        } else {
            next_ctx = sched.main_thread;
        }
        if (sched.cartesian_root && !sched.cartesian_root->is_infinite) {
            oz::x86_64::setLAPICOneShot(sched.cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
        } else {
            oz::x86_64::stopLAPICTimer();
        }
        if (next_ctx) {
            sched.performContextSwitch(current, next_ctx, now);
        }
        return;
    }

    // ケース ③ 委譲チェーン中間の親 Baton が破棄された場合: そのまま正常復帰

    // ケース ④ current->pml4_phys == 0（自身の VSpace が破棄された場合）
    if (current->pml4_phys.get() == 0) {
        current->state = ThreadState::Blocked;
        current->is_blocked = true;
        current->sched_reason = SchedReason::FAULT;

        Baton* b = current->get_active_baton();
        if (!b) b = sched.running_baton;
        ExecutionContext* next_ctx = nullptr;
        if (b) {
            cartesianPopLeaf(sched.cartesian_root, b);
            b->state = BatonState::BLOCKED;
            b->is_active = false;
            Baton* caller_b = b->caller_baton;
            ExecutionContext* caller = b->caller_context;
            b->caller_baton = nullptr;
            b->caller_context = nullptr;
            if (caller_b) {
                caller_b->callee_baton = nullptr;
                caller_b->state = BatonState::RUNNING;
                cartesianPushDown(caller_b);
                sched.running_baton = caller_b;
                if (!caller) caller = caller_b->thread;
            } else if (sched.running_baton == b) {
                sched.running_baton = nullptr;
            }
            if (caller && caller != current && !caller->is_dead) {
                caller->sched_reason = SchedReason::FAULT;
                next_ctx = caller;
            }
        }
        if (!next_ctx) {
            next_ctx = sched.main_thread;
        }
        if (sched.cartesian_root && !sched.cartesian_root->is_infinite) {
            oz::x86_64::setLAPICOneShot(sched.cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
        } else {
            oz::x86_64::stopLAPICTimer();
        }
        if (next_ctx) {
            sched.performContextSwitch(current, next_ctx, now);
        }
        return;
    }

    // ケース ⑤ current->cspace == nullptr（自身の CSpaceCap のみが破棄された場合）: そのまま正常復帰
}

// Get current CSpace (common pattern)
static CSpace* get_cspace() {
    if (!g_get_current_cspace_func) return nullptr;
    return g_get_current_cspace_func();
}

// ===========================================================================
// Channel IPC helpers & Bulk Allocation
// ===========================================================================

constexpr CapID CAP_SELF_CSPACE = 245;

struct SyscallResult {
    std::uint64_t rax{0};
    std::uint64_t rdx{0};

    SyscallResult(std::uint64_t rax = 0, std::uint64_t rdx = 0) : rax(rax), rdx(rdx) {}
    SyscallResult(std::int64_t rax) : rax(static_cast<std::uint64_t>(rax)), rdx(0) {}
    SyscallResult(int rax) : rax(static_cast<std::uint64_t>(static_cast<std::int64_t>(rax))), rdx(0) {}
};

static inline void transfer_or_drop_ipc_cap(
    CSpace* dst_cs, CapID dst_slot,
    CSpace* src_cs, CapID src_id,
    bool& out_has_cap, CapID& out_cap_id
) {
    out_has_cap = false;
    out_cap_id = 0;
    if (!src_cs || !dst_cs) return;

    // 1. 送信側 Cap の妥当性検証 (不正な Cap の場合は破棄も転送もしない)
    if (src_id == 0 || src_id >= CSpace::MAX_CAPS || is_reserved_system_slot(src_id)) {
        return;
    }
    Capability* src = src_cs->getSlot(src_id);
    if (!src || src->is_null() || !src->has_rights(CapRights::TRANSFER)) {
        return;
    }

    // 2. 受信側スロットの検証
    bool dst_slot_valid = (dst_slot != 0 && dst_slot < CSpace::MAX_CAPS &&
                           !is_reserved_system_slot(dst_slot));
    Capability* dest = dst_slot_valid ? dst_cs->getSlot(dst_slot) : nullptr;

    if (dest && dest->is_null()) {
        // 正常転送
        if (cdt_move_slot(dest, src)) {
            out_has_cap = true;
            out_cap_id = dst_slot;
            return;
        }
    }

    // 3. 受信側が受取不能 (0, 衝突, 予約領域等) の場合のみ送信元 Cap を Drop
    src_cs->remove(src_id);
}

static inline CSpace* resolve_target_cspace(CSpace* current_cs, CapID target_cs_id, CapRights rights = CapRights::WRITE) {
    if (!current_cs) return nullptr;
    if (target_cs_id == 0 || target_cs_id == CAP_SELF_CSPACE) {
        return current_cs;
    }
    CSpaceCap* cs_cap = get_cap_typed<CSpaceCap>(current_cs, target_cs_id, ObjectType::CSpaceCap, rights);
    if (!cs_cap) return nullptr;
    return cs_cap->getCSpace();
}

// -----------------------------------------------------------------
// 事前容量シミュレーションヘルパー (アライメント考慮の容量充足性検証)
// -----------------------------------------------------------------
static inline bool untyped_can_allocate_bulk(
    const Capability* ut_slot,
    ObjectType        target_type,
    std::uint64_t     size_or_arg,
    std::size_t       count
) {
    std::uint64_t current_watermark = ut_slot->untyped_watermark();
    std::uint64_t total_size = ut_slot->untyped_size();
    std::uint64_t simulated_wm = current_watermark;

    auto align_up = [](std::uint64_t val, std::uint64_t align) -> std::uint64_t {
        return (val + align - 1) & ~(align - 1);
    };

    for (std::size_t i = 0; i < count; ++i) {
        switch (target_type) {
            case ObjectType::Frame: {
                simulated_wm = align_up(simulated_wm, 4096);
                if (simulated_wm + 4096 > total_size || simulated_wm + 4096 < simulated_wm) return false;
                simulated_wm += 4096;
                break;
            }
            case ObjectType::UntypedMemory: {
                simulated_wm = align_up(simulated_wm, 4096);
                if (simulated_wm + size_or_arg > total_size || simulated_wm + size_or_arg < simulated_wm) return false;
                simulated_wm += size_or_arg;
                break;
            }
            case ObjectType::TCB: {
                simulated_wm = align_up(simulated_wm, 4096);
                if (simulated_wm + 8192 > total_size || simulated_wm + 8192 < simulated_wm) return false;
                simulated_wm += 8192;
                break;
            }
            case ObjectType::ChannelEndpoint: {
                constexpr std::size_t sz = sizeof(ChannelEndpoint);
                constexpr std::size_t al = alignof(ChannelEndpoint);
                simulated_wm = align_up(simulated_wm, al);
                if (simulated_wm + sz > total_size || simulated_wm + sz < simulated_wm) return false;
                simulated_wm += sz;
                break;
            }
            case ObjectType::Notification: {
                constexpr std::size_t sz = sizeof(Notification);
                constexpr std::size_t al = alignof(Notification);
                simulated_wm = align_up(simulated_wm, al);
                if (simulated_wm + sz > total_size || simulated_wm + sz < simulated_wm) return false;
                simulated_wm += sz;
                break;
            }
            case ObjectType::Baton: {
                constexpr std::size_t sz = sizeof(Baton);
                constexpr std::size_t al = alignof(Baton);
                simulated_wm = align_up(simulated_wm, al);
                if (simulated_wm + sz > total_size || simulated_wm + sz < simulated_wm) return false;
                simulated_wm += sz;
                break;
            }
            case ObjectType::CSpaceCap: {
                simulated_wm = align_up(simulated_wm, alignof(CSpace));
                if (simulated_wm + sizeof(CSpace) > total_size || simulated_wm + sizeof(CSpace) < simulated_wm) return false;
                simulated_wm += sizeof(CSpace);

                simulated_wm = align_up(simulated_wm, alignof(CSpaceCap));
                if (simulated_wm + sizeof(CSpaceCap) > total_size || simulated_wm + sizeof(CSpaceCap) < simulated_wm) return false;
                simulated_wm += sizeof(CSpaceCap);
                break;
            }
            default:
                return false;
        }
    }

    return true;
}

// syscall 59: sys_untyped_retype_bulk
SyscallResult handle_untyped_retype_bulk(
    CapID         ut_cap_id,
    ObjectType    target_type,
    std::uint64_t size_or_arg,
    CapID         target_cspace_id,
    CapID         start_slot,
    std::size_t   count
) {
    CSpace* current_cs = get_cspace();
    if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

    // -------------------------------------------------------------
    // Phase 1: 事前全数検証 (Dry-Run Validation)
    // -------------------------------------------------------------
    if (count == 0 || count > CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
    if (start_slot == 0 || start_slot >= CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
    if (count > CSpace::MAX_CAPS - start_slot) return SyscallResult(SYS_ERR_INVALID, 0);

    std::uint32_t end_slot = start_slot + count - 1;
    if (!(end_slot < CAP_RESERVED_SYSTEM_START || start_slot > CAP_RESERVED_SYSTEM_END)) {
        return SyscallResult(SYS_ERR_INVALID, 0);
    }

    if (target_type != ObjectType::Frame &&
        target_type != ObjectType::UntypedMemory &&
        target_type != ObjectType::TCB &&
        target_type != ObjectType::ChannelEndpoint &&
        target_type != ObjectType::Notification &&
        target_type != ObjectType::Baton &&
        target_type != ObjectType::CSpaceCap) {
        return SyscallResult(SYS_ERR_INVALID, 0);
    }

    if (target_type == ObjectType::ChannelEndpoint && (count % 2) != 0) {
        return SyscallResult(SYS_ERR_INVALID, 0);
    }

    if (target_type == ObjectType::UntypedMemory) {
        if (size_or_arg == 0 || (size_or_arg % 4096) != 0) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }
    } else if (target_type == ObjectType::Frame) {
        if (size_or_arg != 0 && size_or_arg != 4096) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }
    }

    Capability* ut_slot = current_cs->getSlot(ut_cap_id);
    if (!ut_slot || ut_slot->type() != CapType::UntypedMemory ||
        (!ut_slot->has_rights(CapRights::WRITE) && !ut_slot->has_rights(CapRights::DERIVE))) {
        return SyscallResult(SYS_ERR_PERM, 0);
    }

    if (ut_slot->untyped_is_device()) {
        if (target_type != ObjectType::Frame && target_type != ObjectType::UntypedMemory) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }
    }

    CSpace* target_cs = resolve_target_cspace(current_cs, target_cspace_id);
    if (!target_cs) {
        return SyscallResult(SYS_ERR_PERM, 0);
    }

    if (!target_cs->is_range_null(start_slot, count)) {
        return SyscallResult(SYS_ERR_BUSY, 0);
    }

    if (!untyped_can_allocate_bulk(ut_slot, target_type, size_or_arg, count)) {
        return SyscallResult(SYS_ERR_NO_MEMORY, 0);
    }

    // -------------------------------------------------------------
    // Phase 2: 一括確定 (No-fail Commit)
    // -------------------------------------------------------------
    if (target_type == ObjectType::Frame) {
        std::uintptr_t phys_base = 0;
        void* mem = untyped_allocate_bytes(ut_slot, count * 4096, 4096, phys_base);
        (void)mem;
        for (std::size_t i = 0; i < count; ++i) {
            Capability* s = target_cs->getSlot(start_slot + i);
            init_frame_slot(s, phys_base + i * 4096, CapRights::ALL, true);
            cdt_insert_child(ut_slot, s);
        }
    } else if (target_type == ObjectType::ChannelEndpoint) {
        for (std::size_t i = 0; i < count; i += 2) {
            ChannelEndpoint* end0 = untyped_allocate_object<ChannelEndpoint>(ut_slot, true);
            ChannelEndpoint* end1 = untyped_allocate_object<ChannelEndpoint>(ut_slot, true);
            end0->set_peer(end1);
            end1->set_peer(end0);

            Capability* s0 = target_cs->getSlot(start_slot + i);
            Capability* s1 = target_cs->getSlot(start_slot + i + 1);
            init_kernel_object_slot(s0, end0, CapRights::ALL);
            init_kernel_object_slot(s1, end1, CapRights::ALL);
            cdt_insert_child(ut_slot, s0);
            cdt_insert_child(ut_slot, s1);
        }
    } else if (target_type == ObjectType::CSpaceCap) {
        for (std::size_t i = 0; i < count; ++i) {
            std::uintptr_t cs_phys = 0;
            void* cs_mem = untyped_allocate_bytes(ut_slot, sizeof(CSpace), alignof(CSpace), cs_phys);
            CSpace* child_cspace = new (cs_mem) CSpace();
            CSpaceCap* cs_cap_obj = untyped_allocate_object<CSpaceCap>(ut_slot, child_cspace, false, true);

            Capability* s = target_cs->getSlot(start_slot + i);
            init_kernel_object_slot(s, cs_cap_obj, CapRights::ALL);
            cdt_insert_child(ut_slot, s);
        }
    } else if (target_type == ObjectType::UntypedMemory) {
        for (std::size_t i = 0; i < count; ++i) {
            std::uintptr_t sub_phys = 0;
            void* sub_mem = untyped_allocate_bytes(ut_slot, size_or_arg, 4096, sub_phys);
            (void)sub_mem;
            Capability* s = target_cs->getSlot(start_slot + i);
            init_untyped_slot(s, sub_phys, size_or_arg, ut_slot->untyped_is_device());
            cdt_insert_child(ut_slot, s);
        }
    } else if (target_type == ObjectType::TCB) {
        for (std::size_t i = 0; i < count; ++i) {
            TCB* tcb = untyped_retype_tcb(ut_slot, g_queue_thread_func);
            Capability* s = target_cs->getSlot(start_slot + i);
            init_kernel_object_slot(s, tcb, CapRights::ALL);
            cdt_insert_child(ut_slot, s);
        }
    } else if (target_type == ObjectType::Notification) {
        for (std::size_t i = 0; i < count; ++i) {
            Notification* notif = untyped_allocate_object<Notification>(ut_slot, true);
            Capability* s = target_cs->getSlot(start_slot + i);
            init_kernel_object_slot(s, notif, CapRights::ALL);
            cdt_insert_child(ut_slot, s);
        }
    } else if (target_type == ObjectType::Baton) {
        for (std::size_t i = 0; i < count; ++i) {
            Baton* baton = untyped_allocate_object<Baton>(ut_slot, true);
            Capability* s = target_cs->getSlot(start_slot + i);
            init_kernel_object_slot(s, baton, CapRights::ALL);
            cdt_insert_child(ut_slot, s);
        }
    }

    return SyscallResult(0, 0);
}

// ===========================================================================
// Syscall handlers
// ===========================================================================

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type-c-linkage"
extern "C" SyscallResult handleSyscall(std::uint64_t rdi, std::uint64_t rsi, std::uint64_t rdx,
                                       std::uint64_t r10, std::uint64_t r8, std::uint64_t r9,
                                       std::uint64_t syscall_number) {
#pragma GCC diagnostic pop

    struct ScopeTlbFlusher {
        ~ScopeTlbFlusher() {
            flush_tlb_if_pending();
        }
    } tlb_flusher;

    if (syscall_number == 3) {
        // channel_create(untyped_cap, target_cs0, slot0, target_cs1, slot1, modes)
        CSpace* current_cs = get_cspace();
        if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

        CapID ut_cap = static_cast<CapID>(rdi);
        CapID target_cs0_id = static_cast<CapID>(rsi);
        CapID slot0 = static_cast<CapID>(rdx);
        CapID target_cs1_id = static_cast<CapID>(r10);
        CapID slot1 = static_cast<CapID>(r8);
        std::uint64_t modes_raw = r9;

        Capability* ut_slot = current_cs->getSlot(ut_cap);
        if (!ut_slot || ut_slot->type() != CapType::UntypedMemory ||
            (!ut_slot->has_rights(CapRights::WRITE) && !ut_slot->has_rights(CapRights::DERIVE))) {
            return SyscallResult(SYS_ERR_PERM, 0);
        }
        if (ut_slot->untyped_is_device()) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }

        CSpace* cs0 = resolve_target_cspace(current_cs, target_cs0_id);
        if (!cs0) return SyscallResult(SYS_ERR_PERM, 0);

        CSpace* cs1 = resolve_target_cspace(current_cs, target_cs1_id);
        if (!cs1) return SyscallResult(SYS_ERR_PERM, 0);

        if (slot0 == 0 || slot0 >= CSpace::MAX_CAPS || slot1 == 0 || slot1 >= CSpace::MAX_CAPS) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }
        if (is_reserved_system_slot(slot0) || is_reserved_system_slot(slot1)) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }
        if (cs0 == cs1 && slot0 == slot1) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }

        Capability* s0 = cs0->getSlot(slot0);
        Capability* s1 = cs1->getSlot(slot1);
        if (!s0 || !s0->is_null() || !s1 || !s1->is_null()) {
            return SyscallResult(SYS_ERR_BUSY, 0);
        }

        ChannelMode mode0 = static_cast<ChannelMode>(modes_raw & 0xFFFFFFFF);
        ChannelMode mode1 = static_cast<ChannelMode>(modes_raw >> 32);
        if (!is_valid_channel_pair(mode0, mode1)) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }

        if (!untyped_can_allocate_bulk(ut_slot, ObjectType::ChannelEndpoint, 0, 2)) {
            return SyscallResult(SYS_ERR_NO_MEMORY, 0);
        }

        if (!sys_channel_create(ut_slot, cs0, slot0, cs1, slot1, mode0, mode1)) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }
        return SyscallResult(0, 0);
    } else if (syscall_number == 4) {
        // sys_pagetable_map_frame(pt_cap, index, frame_cap, flags)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        Capability* pt_slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!pt_slot || pt_slot->type() != CapType::PageTable || !pt_slot->has_rights(CapRights::WRITE)) {
            return -1;
        }

        auto* pt_obj = pt_slot->pagetable_object();
        if (!pt_obj) return -1;

        if (rsi >= 512 || (pt_obj->level() == 4 && rsi >= 256)) {
            return -1;
        }

        Capability* frame_slot = cspace->getSlot(static_cast<CapID>(rdx));
        if (!frame_slot || frame_slot->type() != CapType::Frame || !frame_slot->has_rights(CapRights::READ)) {
            return -1;
        }

        int res = pt_obj->map_frame(rsi, frame_slot, r10);
        return res;
    } else if (syscall_number == 60) {
        // exit
        if (g_get_current_thread_func) {
            Thread* cur = g_get_current_thread_func();
            if (cur) {
                // Safety guard: reject exit if holding borrowed baton
                if (cur->borrowed_baton != nullptr) {
                    return SYS_ERR_BUSY;
                }
                if (cur->pending_server) {
                    if (cur->pending_server->reply_target == cur) {
                        cur->pending_server->reply_target = nullptr;
                    }
                    cur->pending_server = nullptr;
                }
                if (cur->reply_target) {
                    if (cur->reply_target->pending_server == cur) {
                        cur->reply_target->pending_server = nullptr;
                    }
                    cur->reply_target = nullptr;
                }
                if (cur->exit_notif && !cur->exit_notif->isRevoked()) {
                    std::uint64_t status = rdi & 0xFFFFFFFFULL;
                    cur->exit_notif->signal(cur->exit_badge | status);
                }
            }
        }
        dprint("User process exited.\n\r");
        if (g_exit_thread_func) g_exit_thread_func();
        return 0;
    } else if (syscall_number == 6) {
        // debug_print: write string to serial (secured with pointer validation)
        if (!is_user_address(rdi)) return -1;
        const char* str = reinterpret_cast<const char*>(rdi);
        // Print with length limit to prevent runaway reads
        constexpr std::size_t MAX_DEBUG_LEN = 4096;
        for (std::size_t i = 0; i < MAX_DEBUG_LEN; ++i) {
            if (!is_user_address(rdi + i)) break;
            char c = str[i];
            if (c == '\0') break;
            char buf[2] = {c, '\0'};
            dprint(buf);
        }
        return 0;
    } else if (syscall_number == 7) {
        // sys_cap_unmap(mapped_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        Capability* slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!slot || !slot->has_slot_flag(SLOT_FLAG_IS_MAPPED)) {
            return -1;
        }

        if (!unmap_capability_slot(slot)) {
            return -1;
        }
        return 0;
    } else if (syscall_number == 8) {
        // frame_phys_addr
        CSpace* cspace = get_cspace();
        if (!cspace) return 0;
        
        Capability* slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!slot || slot->type() != CapType::Frame || !slot->has_rights(CapRights::READ)) return 0;
        
        return static_cast<std::uint64_t>(slot->frame_phys());
    } else if (syscall_number == 9) {
        // cap_mint(target_cspace, target_slot, src_cap, new_rights, badge)
        CSpace* current_cs = get_cspace();
        if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

        CapID target_cs_id = static_cast<CapID>(rdi);
        CapID target_slot = static_cast<CapID>(rsi);
        CapID src_cap = static_cast<CapID>(rdx);
        CapRights new_rights = static_cast<CapRights>(r10);
        std::uint64_t badge = r8;

        if (target_slot == 0 || target_slot >= CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
        if (is_reserved_system_slot(target_slot)) return SyscallResult(SYS_ERR_INVALID, 0);

        CSpace* target_cs = resolve_target_cspace(current_cs, target_cs_id);
        if (!target_cs) return SyscallResult(SYS_ERR_PERM, 0);

        Capability* target_slot_cap = target_cs->getSlot(target_slot);
        if (!target_slot_cap || !target_slot_cap->is_null()) {
            return SyscallResult(SYS_ERR_BUSY, 0);
        }

        Capability* src_slot = current_cs->getSlot(src_cap);
        if (!src_slot || src_slot->is_null()) return SyscallResult(SYS_ERR_INVALID, 0);

        // Privilege escalation check
        if (new_rights == CapRights::ALL) {
            new_rights = src_slot->rights();
        } else if ((static_cast<std::uint32_t>(new_rights) & ~static_cast<std::uint32_t>(src_slot->rights())) != 0) {
            return SyscallResult(SYS_ERR_PERM, 0);
        }

        if (!cap_copy(target_cs, target_slot, current_cs, src_cap, new_rights, badge, badge != 0)) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }
        return SyscallResult(0, 0);
    } else if (syscall_number == 10) {
        // cap_delete
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        Capability* slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!slot || slot->is_null()) return -1;

        cdt_remove_and_reparent(slot);

        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        handleSelfDestructionIfNeeded(current);
        return 0;
    } else if (syscall_number == 11) {
        // tcb_configure(tcb_cap, cspace_cap, vspace_cap, rip, user_rsp)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        ExecutionContext* target = get_cap_execution_context(
            cspace, static_cast<CapID>(rdi), CapRights::WRITE);
        if (!target) return -1;

        CSpaceCap* target_cspace = get_cap_typed<CSpaceCap>(
            cspace, static_cast<CapID>(rsi), ObjectType::CSpaceCap, CapRights::READ);
        if (!target_cspace) return -1;

        VSpace* target_vspace = get_cap_typed<VSpace>(
            cspace, static_cast<CapID>(rdx), ObjectType::VSpace, CapRights::READ);
        if (!target_vspace) return -1;

        if (!is_user_address(r10)) return -1; // rip
        if (!is_user_address(r8)) return -1;  // user_rsp

        if (!configure_execution_context(target, target_cspace->getCSpace(), target_vspace, r10, r8)) {
            return -1;
        }
        return 0;
    } else if (syscall_number == 13) {
        // tcb_resume
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        ExecutionContext* target = get_cap_execution_context(
            cspace, static_cast<CapID>(rdi), CapRights::WRITE);
        if (!target) return -1;

        target->is_blocked = false;
        target->state = ThreadState::Ready;

        Thread* t = static_cast<Thread*>(target);
        if (t->sched_notif && t->sched_badge != 0) {
            t->sched_notif->signal(t->sched_badge);
            return 0;
        }

        if (target->baton) {
            target->baton->notify_if_runnable();
        }
        return 0;
    } else if (syscall_number == 14) {
        // tcb_suspend
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        ExecutionContext* target = get_cap_execution_context(
            cspace, static_cast<CapID>(rdi), CapRights::WRITE);
        if (!target) return -1;
        
        target->is_blocked = true;
        return 0;
    } else if (syscall_number == 16) {
        // cspace_cap_insert(target_cspace_cap, target_slot, src_cap_in_current_cspace, rights)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        CSpaceCap* target_cs = get_cap_typed<CSpaceCap>(
            cspace, static_cast<CapID>(rdi), ObjectType::CSpaceCap, CapRights::WRITE);
        if (!target_cs || !target_cs->getCSpace()) return -1;

        CapID target_slot = static_cast<CapID>(rsi);
        CapID src_slot = static_cast<CapID>(rdx);
        CapRights new_rights = static_cast<CapRights>(r10);

        if (!cap_copy(target_cs->getCSpace(), target_slot, cspace, src_slot, new_rights)) {
            return -1;
        }
        return 0;
    } else if (syscall_number == 17) {
        // sys_pagetable_create(untyped_cap, frame_cap, level, target_cspace, target_slot)
        CSpace* current_cs = get_cspace();
        if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

        std::uint32_t level = static_cast<std::uint32_t>(rdx);
        if (level < 1 || level > 4) return SyscallResult(SYS_ERR_INVALID, 0);

        CapID target_cs_id = static_cast<CapID>(r10);
        CapID target_slot = static_cast<CapID>(r8);
        if (target_slot == 0 || target_slot >= CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
        if (is_reserved_system_slot(target_slot)) return SyscallResult(SYS_ERR_INVALID, 0);

        CSpace* target_cs = resolve_target_cspace(current_cs, target_cs_id);
        if (!target_cs) return SyscallResult(SYS_ERR_PERM, 0);

        Capability* target_slot_cap = target_cs->getSlot(target_slot);
        if (!target_slot_cap || !target_slot_cap->is_null()) {
            return SyscallResult(SYS_ERR_BUSY, 0);
        }

        Capability* ut_slot = current_cs->getSlot(static_cast<CapID>(rdi));
        if (!ut_slot || ut_slot->type() != CapType::UntypedMemory ||
            (!ut_slot->has_rights(CapRights::WRITE) && !ut_slot->has_rights(CapRights::DERIVE))) {
            return SyscallResult(SYS_ERR_PERM, 0);
        }
        if (ut_slot->untyped_is_device()) return SyscallResult(SYS_ERR_INVALID, 0);

        Capability* frame_slot = current_cs->getSlot(static_cast<CapID>(rsi));
        if (!PageTableObject::validate_backing_frame(frame_slot)) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }

        std::size_t saved_watermark = ut_slot->untyped_watermark();
        auto* pt_obj = untyped_allocate_object<PageTableObject>(ut_slot, static_cast<std::uint8_t>(level));
        if (!pt_obj) {
            ut_slot->set_untyped_watermark(saved_watermark);
            return SyscallResult(SYS_ERR_NO_MEMORY, 0);
        }

        pt_obj->bind_backing_frame(frame_slot, static_cast<std::uint8_t>(level));
        init_pagetable_object_slot(target_slot_cap, pt_obj, static_cast<std::uint8_t>(level));
        cdt_insert_child(ut_slot, target_slot_cap);
        return SyscallResult(0, 0);
    } else if (syscall_number == 18) {
        // irq_handler_create(untyped_cap, vector, notification_ep_cap, target_cspace, target_slot)
        CSpace* current_cs = get_cspace();
        if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

        std::uint8_t vector = static_cast<std::uint8_t>(rsi);
        // Whitelist check: only allow external IRQs defined in IDT (0x30 - 0x4F)
        if (vector < 0x30 || vector > 0x4F) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }
        // Conflict check: vector must not be already in use
        if (oz::irq::getHandler(vector) != nullptr && oz::irq::getHandler(vector)->isRegistered()) {
            return SyscallResult(SYS_ERR_BUSY, 0);
        }

        CapID target_cs_id = static_cast<CapID>(r10);
        CapID target_slot = static_cast<CapID>(r8);
        if (target_slot == 0 || target_slot >= CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
        if (is_reserved_system_slot(target_slot)) return SyscallResult(SYS_ERR_INVALID, 0);

        CSpace* target_cs = resolve_target_cspace(current_cs, target_cs_id);
        if (!target_cs) return SyscallResult(SYS_ERR_PERM, 0);

        Capability* target_slot_cap = target_cs->getSlot(target_slot);
        if (!target_slot_cap || !target_slot_cap->is_null()) {
            return SyscallResult(SYS_ERR_BUSY, 0);
        }

        Capability* ut_slot = nullptr;
        if (rdi != 0 && rdi != INVALID_CAP_ID) {
            ut_slot = current_cs->getSlot(static_cast<CapID>(rdi));
            if (!ut_slot || ut_slot->type() != CapType::UntypedMemory || (!ut_slot->has_rights(CapRights::WRITE) && !ut_slot->has_rights(CapRights::DERIVE))) {
                return SyscallResult(SYS_ERR_PERM, 0);
            }
        }

        IRQHandler& handler = oz::g_irq_handlers[vector];
        handler.vector = vector;
        handler.unregisterAndReset();

        if (rdx != 0 && rdx != INVALID_CAP_ID) {
            Capability* ep_or_notif_slot = current_cs->getSlot(static_cast<CapID>(rdx));
            if (ep_or_notif_slot && !ep_or_notif_slot->is_null()) {
                if (ep_or_notif_slot->type() == CapType::ChannelEndpoint) {
                    handler.setNotificationEndpoint(static_cast<ChannelEndpoint*>(ep_or_notif_slot->object()));
                } else if (ep_or_notif_slot->type() == CapType::Notification) {
                    handler.setNotification(static_cast<Notification*>(ep_or_notif_slot->object()), ep_or_notif_slot->badge());
                }
            }
        }
        handler.registerHandler();

        init_irq_handler_slot(target_slot_cap, vector, &handler);
        if (ut_slot) {
            cdt_insert_child(ut_slot, target_slot_cap);
        }
        return SyscallResult(0, 0);
    } else if (syscall_number == 19) {
        // irq_ack
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        Capability* slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!slot || slot->type() != CapType::IRQHandler || !slot->has_rights(CapRights::ACK_WAIT)) {
            return -1;
        }
        std::uint8_t vector = slot->irq_vector();
        IRQHandler& handler = oz::g_irq_handlers[vector];
        if (!handler.acknowledge()) return -1;
        return 0;
    } else if (syscall_number == 20) {
        // ioport_create(parent_ioport_cap, base_port, port_count, target_cspace, target_slot)
        CSpace* current_cs = get_cspace();
        if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

        Capability* parent_slot = current_cs->getSlot(static_cast<CapID>(rdi));
        if (!parent_slot || parent_slot->type() != CapType::IOPort || !parent_slot->has_rights(CapRights::DERIVE)) {
            return SyscallResult(SYS_ERR_PERM, 0);
        }

        std::uint16_t parent_base = parent_slot->ioport_base();
        std::uint32_t parent_count = parent_slot->ioport_count();

        std::uint16_t base_port = static_cast<std::uint16_t>(rsi);
        std::uint32_t port_count = static_cast<std::uint32_t>(rdx);

        if (port_count == 0 || base_port < parent_base ||
            static_cast<std::uint32_t>(base_port) + port_count > static_cast<std::uint32_t>(parent_base) + parent_count) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }

        CapID target_cs_id = static_cast<CapID>(r10);
        CapID target_slot = static_cast<CapID>(r8);
        if (target_slot == 0 || target_slot >= CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
        if (is_reserved_system_slot(target_slot)) return SyscallResult(SYS_ERR_INVALID, 0);

        CSpace* target_cs = resolve_target_cspace(current_cs, target_cs_id);
        if (!target_cs) return SyscallResult(SYS_ERR_PERM, 0);

        Capability* target_slot_cap = target_cs->getSlot(target_slot);
        if (!target_slot_cap || !target_slot_cap->is_null()) {
            return SyscallResult(SYS_ERR_BUSY, 0);
        }

        init_ioport_slot(target_slot_cap, base_port, port_count);
        target_slot_cap->set_rights(parent_slot->rights());
        cdt_insert_child(parent_slot, target_slot_cap);
        return SyscallResult(0, 0);
    } else if (syscall_number == 21) {
        // ioport_read(ioport_cap, port, size)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        Capability* slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!slot || slot->type() != CapType::IOPort || !slot->has_rights(CapRights::IN)) {
            return -1;
        }

        std::uint16_t base_port = slot->ioport_base();
        std::uint32_t port_count = slot->ioport_count();
        std::uint16_t port = static_cast<std::uint16_t>(rsi);
        std::uint64_t size = rdx;

        if (port < base_port || static_cast<std::uint32_t>(port) + size > static_cast<std::uint32_t>(base_port) + port_count) {
            return -1;
        }

        if (size == 1) {
            return static_cast<std::uint64_t>(oz::x86_64::readIO8(port));
        } else if (size == 2) {
            return static_cast<std::uint64_t>(oz::x86_64::readIO16(port));
        } else if (size == 4) {
            return static_cast<std::uint64_t>(oz::x86_64::readIO32(port));
        }
        return -1;
    } else if (syscall_number == 22) {
        // ioport_write(ioport_cap, port, value, size)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        Capability* slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!slot || slot->type() != CapType::IOPort || !slot->has_rights(CapRights::OUT)) {
            return -1;
        }

        std::uint16_t base_port = slot->ioport_base();
        std::uint32_t port_count = slot->ioport_count();
        std::uint16_t port = static_cast<std::uint16_t>(rsi);
        std::uint64_t size = r10;

        if (port < base_port || static_cast<std::uint32_t>(port) + size > static_cast<std::uint32_t>(base_port) + port_count) {
            return -1;
        }

        if (size == 1) {
            oz::x86_64::writeIO8(port, static_cast<std::uint8_t>(rdx));
        } else if (size == 2) {
            oz::x86_64::writeIO16(port, static_cast<std::uint16_t>(rdx));
        } else if (size == 4) {
            oz::x86_64::writeIO32(port, static_cast<std::uint32_t>(rdx));
        } else {
            return -1;
        }
        return 0;
    } else if (syscall_number == 24) {
        // untyped_revoke(untyped_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        Capability* ut_slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!ut_slot || ut_slot->type() != CapType::UntypedMemory || !ut_slot->has_rights(CapRights::REVOKE)) {
            return -1;
        }

        cdt_revoke_subtree(ut_slot);

        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        handleSelfDestructionIfNeeded(current);
        return 0;
    } else if (syscall_number == 25) {
        // sys_pagetable_map(parent_pt_cap, index, child_pt_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        Capability* parent_slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!parent_slot || (parent_slot->type() != CapType::PageTable && parent_slot->type() != CapType::VSpace) || !parent_slot->has_rights(CapRights::WRITE)) {
            return -1;
        }

        auto* parent_pt = parent_slot->pagetable_object();
        if (!parent_pt) return -1;

        if (rsi >= 512 || (parent_pt->level() == 4 && rsi >= 256)) {
            return -1;
        }

        Capability* child_slot = cspace->getSlot(static_cast<CapID>(rdx));
        if (!child_slot || (child_slot->type() != CapType::PageTable && child_slot->type() != CapType::VSpace)) {
            return -1;
        }

        int res = parent_pt->map_child_table(rsi, child_slot);
        return res;
    } else if (syscall_number == 26) {
        // frame_size(frame_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return 0;

        Capability* frame_slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!frame_slot || frame_slot->type() != CapType::Frame || !frame_slot->has_rights(CapRights::READ)) {
            return 0;
        }

        return static_cast<std::uint64_t>(frame_slot->frame_size());
    } else if (syscall_number == 27) {
        // sys_call(ep_cap, send_msg, recv_msg, flags)
        // rdi: ep_cap, rsi: send_msg, rdx: recv_msg, r10: flags
        CSpace* cspace = get_cspace();
        if (!cspace) return SYS_ERR_INVALID;
        if (!is_user_buffer(rsi, sizeof(IPCMessage))) return SYS_ERR_INVALID;
        if (!is_user_buffer(rdx, sizeof(IPCMessage))) return SYS_ERR_INVALID;

        Capability* ep_slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!ep_slot || ep_slot->is_null()) return SYS_ERR_INVALID;

        ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
            cspace, static_cast<CapID>(rdi), ObjectType::ChannelEndpoint, CapRights::WRITE);
        if (!ep || ep->is_peer_closed() || !ep->get_peer()) return SYS_ERR_INVALID;
        ChannelEndpoint* peer = ep->get_peer();

        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        if (!current || !current->has_baton()) {
            dprint("[KERNEL PANIC] Invariant violation (C-7): sys_call invoked without active baton! tid=");
            if (current) dprint_dec(current->id);
            dprint("\n\r");
            while (true) {
                __asm__ volatile("cli; hlt");
            }
        }

        std::uint32_t flags = static_cast<std::uint32_t>(r10);

        // Check SEND_NONBLOCK mode
        if (ep->get_mode() == ChannelMode::SEND_NONBLOCK && !(flags & IPC_FLAG_BLOCK)) {
            if (!peer->has_recv_waiters()) {
                return SYS_ERR_WOULDBLOCK;
            }
        }

        const IPCMessage* msg = reinterpret_cast<const IPCMessage*>(rsi);
        InternalIPCMessage internal;
        internal.user_msg = *msg;
        internal.user_msg.badge = ep_slot->badge();
        if (msg->has_cap) {
            if (!ep_slot->has_rights(CapRights::GRANT)) return SYS_ERR_INVALID;
            if (msg->transferred_cap == 0 || msg->transferred_cap >= CSpace::MAX_CAPS ||
                is_reserved_system_slot(msg->transferred_cap)) {
                return SYS_ERR_INVALID;
            }
            Capability* src_cap = cspace->getSlot(msg->transferred_cap);
            if (!src_cap || src_cap->is_null() || !src_cap->has_rights(CapRights::TRANSFER)) return SYS_ERR_INVALID;
        }

        current->recv_msg_buf = reinterpret_cast<IPCMessage*>(rdx);
        current->blocking_ep = ep;
        current->recv_slot = static_cast<CapID>(r8);

        // Baton to lend is current thread's active baton
        Baton* baton_to_lend = current->get_active_baton();

        // Check if receiver is already waiting on peer (Recv 先行 -> Call 到着)
        Thread* receiver = peer->pop_recv_waiter();
        if (receiver) {
            if (internal.user_msg.has_cap) {
                bool out_has_cap = false;
                CapID out_cap = 0;
                transfer_or_drop_ipc_cap(receiver->cspace, receiver->recv_slot, cspace, internal.user_msg.transferred_cap, out_has_cap, out_cap);
                internal.user_msg.transferred_cap = out_cap;
                internal.user_msg.has_cap = out_has_cap;
            } else {
                internal.user_msg.has_cap = false;
            }
            receiver->kernel_ipc_msg = internal.user_msg;
            receiver->has_kernel_ipc_msg = true;

            // Link caller and callee
            receiver->reply_target = current;
            current->pending_server = receiver;

            // Hand-off baton from client to server (borrowed_baton)
            if (current->borrowed_baton == baton_to_lend) {
                current->borrowed_baton = nullptr;
            }
            receiver->borrowed_baton = baton_to_lend;
            if (baton_to_lend) {
                baton_to_lend->thread = receiver;
                baton_to_lend->is_active = true;
            }
            current->baton = current->get_active_baton();
            receiver->baton = receiver->get_active_baton();

            current->state = ThreadState::BlockedOnCall;
            current->is_blocked = true;

            receiver->state = ThreadState::Ready;
            receiver->is_blocked = false;
            if (g_unblock_thread_func) g_unblock_thread_func(receiver);

            std::uint64_t now = oz::x86_64::rdtsc();
            Kernel::instance().getScheduler().performContextSwitch(current, receiver, now);

            if (current->has_kernel_ipc_msg) {
                if (current->recv_msg_buf) {
                    *current->recv_msg_buf = current->kernel_ipc_msg;
                }
                current->has_kernel_ipc_msg = false;
            }

            current->blocking_ep = nullptr;
            if (current->ipc_error != 0) {
                int err = current->ipc_error;
                current->ipc_error = 0;
                return SyscallResult(err, 0);
            }
            if (current->ipc_cancelled) {
                current->ipc_cancelled = false;
                return SYS_ERR_CANCELLED;
            }
            return 0;
        }

        // Server not waiting (Call 先行 -> Recv 到着)
        current->send_msg = internal;
        current->state = ThreadState::BlockedOnCall;
        current->is_blocked = true;
        ep->add_send_waiter(current);

        // Notify peer if bound
        if (peer->get_bound_notification()) {
            peer->get_bound_notification()->signal(peer->get_bound_badge());
        }

        // Time freezing & yield to scheduler
        std::uint64_t now = oz::x86_64::rdtsc();
        if (baton_to_lend && !baton_to_lend->is_infinite) {
            std::uint64_t slice = (now >= current->last_switched_in) ? (now - current->last_switched_in) : 0;
            std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
            std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
            if (baton_to_lend->remaining > consumed_us) {
                baton_to_lend->remaining -= consumed_us;
            } else {
                baton_to_lend->remaining = 0;
            }
            cartesianPushDown(baton_to_lend);
        }
        current->sched_reason = SchedReason::BLOCKED;

        Kernel::instance().getScheduler().yieldToSchedulerBlocked(static_cast<Thread*>(current), now);

        if (current->has_kernel_ipc_msg) {
            if (current->recv_msg_buf) {
                *current->recv_msg_buf = current->kernel_ipc_msg;
            }
            current->has_kernel_ipc_msg = false;
        }

        current->blocking_ep = nullptr;
        if (current->ipc_error != 0) {
            int err = current->ipc_error;
            current->ipc_error = 0;
            return SyscallResult(err, 0);
        }
        if (current->ipc_cancelled) {
            current->ipc_cancelled = false;
            return SYS_ERR_CANCELLED;
        }
        return 0;
    } else if (syscall_number == 28) {
        // sys_reply_and_recv(ep_cap, reply_msg, recv_msg, flags)
        // rdi: ep_cap, rsi: reply_msg, rdx: recv_msg, r10: flags
        CSpace* cspace = get_cspace();
        if (!cspace) return SYS_ERR_INVALID;

        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        if (!current) return SYS_ERR_INVALID;

        CapID ep_cap_id = static_cast<CapID>(rdi);
        const IPCMessage* reply_msg = reinterpret_cast<const IPCMessage*>(rsi);
        IPCMessage* recv_msg = reinterpret_cast<IPCMessage*>(rdx);
        std::uint32_t flags = static_cast<std::uint32_t>(r10);
        CapID recv_slot = static_cast<CapID>(r8);

        if (rsi != 0 && !is_user_buffer(rsi, sizeof(IPCMessage))) {
            return SYS_ERR_INVALID;
        }
        if (rdx != 0 && !is_user_buffer(rdx, sizeof(IPCMessage))) {
            return SYS_ERR_INVALID;
        }

        Thread* client = current->reply_target;
        Baton* b_baton = current->borrowed_baton;

        // 4-state invariant check:
        // State 4: client != nullptr && b_baton == nullptr (No. 11: Invariant violation)
        if (client != nullptr && b_baton == nullptr) {
            dprint("[KERNEL PANIC] Invariant violation: reply_target != nullptr but borrowed_baton == nullptr!\n\r");
            while (true) {
                __asm__ volatile("cli; hlt");
            }
        }

        enum class NextAction {
            NONE,
            DIRECT_SWITCH_CLIENT,
            YIELD_TO_SCHEDULER_BLOCKED,
            YIELD_TO_SCHEDULER_ORPHANED,
            CONTINUE_SELF
        };
        NextAction action = NextAction::NONE;
        Thread* client_to_switch = nullptr;
        Baton* orphaned_b = nullptr;

        // =========================================================================
        // Phase 1: Reply Phase
        // =========================================================================
        // State 3: client == nullptr && b_baton != nullptr (No. 10: Orphaned State)
        if (client == nullptr && b_baton != nullptr) {
            orphaned_b = b_baton;
            current->borrowed_baton = nullptr;
            current->baton = current->get_active_baton();
            action = NextAction::YIELD_TO_SCHEDULER_ORPHANED;
        }
        // State 2: client != nullptr && b_baton != nullptr (No. 4,5,6,7,8,9: Valid RPC Pair)
        else if (client != nullptr && b_baton != nullptr) {
            // Deliver reply if reply_msg != nullptr
            if (reply_msg != nullptr) {
                if (reply_msg->has_cap) {
                    bool ep_authorized = false;
                    if (ep_cap_id != 0 && ep_cap_id != INVALID_CAP_ID) {
                        Capability* ep_slot = cspace->getSlot(ep_cap_id);
                        if (ep_slot && ep_slot->type() == CapType::ChannelEndpoint &&
                            (ep_slot->has_rights(CapRights::GRANT_REPLY) || ep_slot->has_rights(CapRights::GRANT))) {
                            ep_authorized = true;
                        }
                    }
                    if (!ep_authorized && client->blocking_ep) {
                        ChannelEndpoint* peer_ep = client->blocking_ep->get_peer();
                        for (std::size_t i = 0; i < CSpace::MAX_CAPS; ++i) {
                            Capability* slot = cspace->getSlot(i);
                            if (slot && slot->type() == CapType::ChannelEndpoint &&
                                reinterpret_cast<ChannelEndpoint*>(slot->object()) == peer_ep) {
                                if (slot->has_rights(CapRights::GRANT_REPLY) || slot->has_rights(CapRights::GRANT)) {
                                    ep_authorized = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (!ep_authorized) {
                        return SYS_ERR_INVALID;
                    }

                    if (reply_msg->transferred_cap == 0 || reply_msg->transferred_cap >= CSpace::MAX_CAPS ||
                        is_reserved_system_slot(reply_msg->transferred_cap)) {
                        return SYS_ERR_INVALID;
                    }
                    Capability* src_cap = cspace->getSlot(reply_msg->transferred_cap);
                    if (!src_cap || src_cap->is_null() || !src_cap->has_rights(CapRights::TRANSFER)) {
                        return SYS_ERR_INVALID;
                    }
                }

                IPCMessage reply_to_send = *reply_msg;
                if (reply_msg->has_cap) {
                    bool out_has_cap = false;
                    CapID out_cap = 0;
                    transfer_or_drop_ipc_cap(client->cspace, client->recv_slot, cspace, reply_msg->transferred_cap, out_has_cap, out_cap);
                    reply_to_send.transferred_cap = out_cap;
                    reply_to_send.has_cap = out_has_cap;
                } else {
                    reply_to_send.has_cap = false;
                }
                client->kernel_ipc_msg = reply_to_send;
                client->has_kernel_ipc_msg = true;
            }

            // Return borrowed_baton to client
            current->borrowed_baton = nullptr;
            current->reply_target = nullptr;
            current->baton = current->get_active_baton();

            client->pending_server = nullptr;
            // If client was waiting in nested RPC, client restores borrowed_baton
            if (client->reply_target != nullptr) {
                client->borrowed_baton = b_baton;
            } else {
                client->own_baton = b_baton;
            }
            client->baton = client->get_active_baton();
            b_baton->thread = client;
            b_baton->is_active = true;
            client->state = ThreadState::Ready;
            client->is_blocked = false;
            if (g_unblock_thread_func) g_unblock_thread_func(client);
            client_to_switch = client;
            action = NextAction::DIRECT_SWITCH_CLIENT;
        }
        // State 1: client == nullptr && b_baton == nullptr (No. 1,2,3: Clean State)
        // action remains NextAction::NONE

        // =========================================================================
        // Phase 2: Recv Phase
        // =========================================================================
        bool recv_nonblock_wouldblock = false;

        // Recv check (if ep and recv_msg buffer specified)
        if (ep_cap_id != 0 && ep_cap_id != INVALID_CAP_ID && recv_msg != nullptr) {
            ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
                cspace, ep_cap_id, ObjectType::ChannelEndpoint, CapRights::READ);
            if (!ep) {
                if (client_to_switch && current->own_baton == nullptr) {
                    Kernel::instance().getScheduler().performContextSwitch(current, client_to_switch, oz::x86_64::rdtsc());
                }
                return SYS_ERR_INVALID;
            }
            ChannelEndpoint* peer = ep->get_peer();

            // Check if a client is already waiting on peer
            Thread* sender = peer ? peer->pop_send_waiter() : nullptr;

            if (sender) {
                *recv_msg = sender->send_msg.user_msg;
                if (sender->send_msg.user_msg.has_cap) {
                    bool out_has_cap = false;
                    CapID out_cap = 0;
                    transfer_or_drop_ipc_cap(cspace, recv_slot, sender->cspace, sender->send_msg.user_msg.transferred_cap, out_has_cap, out_cap);
                    recv_msg->transferred_cap = out_cap;
                    recv_msg->has_cap = out_has_cap;
                } else {
                    recv_msg->has_cap = false;
                }

                // Lend sender's active baton to current as borrowed_baton
                Baton* sender_baton = sender->get_active_baton();
                current->borrowed_baton = sender_baton;
                current->reply_target = sender;
                sender->pending_server = current;
                if (sender_baton) {
                    sender_baton->thread = current;
                    sender_baton->is_active = true;
                }
                if (sender->borrowed_baton == sender_baton) {
                    sender->borrowed_baton = nullptr;
                }
                sender->baton = sender->get_active_baton();
                sender->state = ThreadState::BlockedOnCall;
                sender->is_blocked = true;

                current->baton = current->get_active_baton();
                current->state = ThreadState::Ready;
                current->is_blocked = false;

                if (sender_baton) {
                    sender_baton->notify_if_runnable();
                }

                // Re-signal bound notification if more senders are waiting on peer
                if (peer->has_send_waiters() && ep->get_bound_notification()) {
                    ep->get_bound_notification()->signal(ep->get_bound_badge());
                }

                if (action == NextAction::NONE) {
                    // 【No. 1 の修正ポイント】
                    // 返信を行っていなかった場合（own_baton で走行中）：
                    // クライアントの要求はクライアントのバトンで実行されるべきなので、
                    // 自身の own_baton の使用を終了し、BLOCKED でスケジューラへ戻る！
                    action = NextAction::YIELD_TO_SCHEDULER_BLOCKED;
                }
                // ※ action == DIRECT_SWITCH_CLIENT の場合（No. 4, No. 7）は、
                //    action を上書きせず DIRECT_SWITCH_CLIENT を維持する！
                //    （直前クライアントへスイッチし、今回受信した sender は後で Sched がディスパッチする）
            } else {
                // --- No sender waiting on peer ---
                bool is_nonblock = (ep->get_mode() == ChannelMode::RECV_NONBLOCK) && !(flags & IPC_FLAG_BLOCK);

                if (is_nonblock) {
                    recv_nonblock_wouldblock = true;
                    current->state = ThreadState::Ready;
                    current->is_blocked = false;

                    if (current->own_baton && current->own_baton->bound_notif && current->own_baton->bound_badge != 0) {
                        current->own_baton->bound_notif->signal(current->own_baton->bound_badge);
                    }

                    if (action == NextAction::NONE) {
                        action = NextAction::CONTINUE_SELF;
                    }
                } else {
                    current->state = ThreadState::BlockedOnRecv;
                    current->is_blocked = true;
                    current->blocking_ep = ep;
                    current->recv_msg_buf = recv_msg;
                    current->recv_slot = recv_slot;
                    ep->add_recv_waiter(current);

                    if (action == NextAction::NONE) {
                        action = NextAction::YIELD_TO_SCHEDULER_BLOCKED;
                    }
                }
            }
        } else {
            // Reply-only check (if no ep or no recv_msg buffer specified)
            if (action == NextAction::NONE) {
                action = NextAction::CONTINUE_SELF;
            }
        }

        // =========================================================================
        // Phase 3: Integration & Switch Phase
        // =========================================================================
        std::uint64_t now = oz::x86_64::rdtsc();

        switch (action) {
            case NextAction::CONTINUE_SELF:
                return recv_nonblock_wouldblock ? SYS_ERR_WOULDBLOCK : 0;

            case NextAction::DIRECT_SWITCH_CLIENT: {
                if (client_to_switch) {
                    Kernel::instance().getScheduler().performContextSwitch(current, client_to_switch, now);
                }

                if (current->has_kernel_ipc_msg) {
                    if (current->recv_msg_buf) {
                        *current->recv_msg_buf = current->kernel_ipc_msg;
                    }
                    current->has_kernel_ipc_msg = false;
                }
                current->blocking_ep = nullptr;
                if (current->ipc_error != 0) {
                    int err = current->ipc_error;
                    current->ipc_error = 0;
                    return SyscallResult(err, 0);
                }
                if (current->ipc_cancelled) {
                    current->ipc_cancelled = false;
                    return SYS_ERR_CANCELLED;
                }
                if (recv_nonblock_wouldblock) {
                    return SYS_ERR_WOULDBLOCK;
                }
                return 0;
            }

            case NextAction::YIELD_TO_SCHEDULER_BLOCKED: {
                Baton* b = current->own_baton ? current->own_baton : current->baton;
                if (b && !b->is_infinite) {
                    std::uint64_t slice = (now >= current->last_switched_in) ? (now - current->last_switched_in) : 0;
                    std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
                    std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
                    if (b->remaining > consumed_us) {
                        b->remaining -= consumed_us;
                    } else {
                        b->remaining = 0;
                    }
                    cartesianPushDown(b);
                }
                current->sched_reason = SchedReason::BLOCKED;
                Kernel::instance().getScheduler().yieldToSchedulerBlocked(static_cast<Thread*>(current), now, b);

                if (current->has_kernel_ipc_msg) {
                    if (current->recv_msg_buf) {
                        *current->recv_msg_buf = current->kernel_ipc_msg;
                    }
                    current->has_kernel_ipc_msg = false;
                }
                current->blocking_ep = nullptr;
                if (current->ipc_error != 0) {
                    int err = current->ipc_error;
                    current->ipc_error = 0;
                    return SyscallResult(err, 0);
                }
                if (current->ipc_cancelled) {
                    current->ipc_cancelled = false;
                    return SYS_ERR_CANCELLED;
                }
                return 0;
            }

            case NextAction::YIELD_TO_SCHEDULER_ORPHANED: {
                if (orphaned_b && !orphaned_b->is_infinite) {
                    std::uint64_t slice = (now >= current->last_switched_in) ? (now - current->last_switched_in) : 0;
                    std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
                    std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
                    if (orphaned_b->remaining > consumed_us) {
                        orphaned_b->remaining -= consumed_us;
                    } else {
                        orphaned_b->remaining = 0;
                    }
                    cartesianPushDown(orphaned_b);
                }
                current->sched_reason = SchedReason::ORPHANED;
                Kernel::instance().getScheduler().yieldToSchedulerOrphaned(static_cast<Thread*>(current), orphaned_b, now);

                if (current->has_kernel_ipc_msg) {
                    if (current->recv_msg_buf) {
                        *current->recv_msg_buf = current->kernel_ipc_msg;
                    }
                    current->has_kernel_ipc_msg = false;
                }
                current->blocking_ep = nullptr;
                if (current->ipc_error != 0) {
                    int err = current->ipc_error;
                    current->ipc_error = 0;
                    return SyscallResult(err, 0);
                }
                if (current->ipc_cancelled) {
                    current->ipc_cancelled = false;
                    return SYS_ERR_CANCELLED;
                }
                return SYS_ERR_WOULDBLOCK;
            }

            default:
                return SYS_ERR_INVALID;
        }
    } else if (syscall_number == 29) {
        // sys_tcb_set_fault_endpoint(tcb_cap, ep_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        TCB* tcb = get_cap_typed<TCB>(
            cspace, static_cast<CapID>(rdi), ObjectType::TCB, CapRights::WRITE);
        if (!tcb) return -1;

        ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
            cspace, static_cast<CapID>(rsi), ObjectType::ChannelEndpoint, CapRights::WRITE | CapRights::READ);
        if (!ep) return -1;

        tcb->setFaultEndpoint(ep);
        return 0;
    } else if (syscall_number == 30) {
        // cap_move(dest_slot, src_slot)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        CapID dest = static_cast<CapID>(rdi);
        CapID src = static_cast<CapID>(rsi);
        if (!cap_move(cspace, dest, src)) return -1;
        return 0;
    } else if (syscall_number == 31) {
        // cspace_cap_move(target_cspace_cap, target_slot, src_slot)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        CSpaceCap* target_cs = get_cap_typed<CSpaceCap>(
            cspace, static_cast<CapID>(rdi), ObjectType::CSpaceCap, CapRights::WRITE);
        if (!target_cs || !target_cs->getCSpace()) return -1;

        CapID target_slot = static_cast<CapID>(rsi);
        CapID src_slot = static_cast<CapID>(rdx);
        if (!cap_move(target_cs->getCSpace(), target_slot, cspace, src_slot)) return -1;
        return 0;
    } else if (syscall_number == 32) {
        // cap_copy(dest_slot, src_slot, rights)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        CapID dest = static_cast<CapID>(rdi);
        CapID src = static_cast<CapID>(rsi);
        CapRights rights = static_cast<CapRights>(rdx);
        if (!cap_copy(cspace, dest, src, rights)) return -1;
        return 0;
    } else if (syscall_number == 35) {
        // notification_signal(notif_cap, badge)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        Notification* notif = get_cap_typed<Notification>(
            cspace, static_cast<CapID>(rdi), ObjectType::Notification, CapRights::WRITE);
        if (!notif) return -1;
        notif->signal(rsi);
        return 0;
    } else if (syscall_number == 36) {
        // notification_wait(notif_cap, out_badge_ptr)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(rsi, sizeof(std::uint64_t))) return -1;
        Notification* notif = get_cap_typed<Notification>(
            cspace, static_cast<CapID>(rdi), ObjectType::Notification, CapRights::READ);
        if (!notif) return -1;

        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        if (!current) return -1;
        if (current->is_irq_handling()) {
            return static_cast<std::uint64_t>(SYS_ERR_INVALID_STATE);
        }

        while (true) {
            __asm__ volatile("cli");
            if (notif->has_signals()) {
                __asm__ volatile("sti");
                current->is_blocked = false;
                *reinterpret_cast<std::uint64_t*>(rsi) = notif->get_and_clear_value();
                return 0;
            }

            current->state = ThreadState::Blocked;
            current->is_blocked = true;
            notif->add_waiter(current);

            std::uint64_t now = oz::x86_64::rdtsc();
            Baton* baton = current->get_active_baton();
            if (baton && !baton->is_infinite) {
                std::uint64_t slice = (now >= current->last_switched_in) ? (now - current->last_switched_in) : 0;
                std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
                std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
                if (baton->remaining > consumed_us) {
                    baton->remaining -= consumed_us;
                } else {
                    baton->remaining = 0;
                }
                cartesianPushDown(baton);
            }
            __asm__ volatile("sti");
            if (baton && (baton->caller_context || baton->caller_baton)) {
                Kernel::instance().getScheduler().yieldToSchedulerBlocked(static_cast<Thread*>(current), now, baton);
            } else if (g_block_thread_func) {
                current->sched_reason = SchedReason::BLOCKED;
                g_block_thread_func();
            } else {
                // 最底辺バトン (Root Baton / sched) の休眠: 自コンテキスト内でインライン HLT 待機
                Kernel::instance().getScheduler().reapZombies();
                __asm__ volatile("sti; hlt");
            }
        }
    } else if (syscall_number == 37) {
        // channel_bind_notification(ep_cap, notif_cap, badge)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
            cspace, static_cast<CapID>(rdi), ObjectType::ChannelEndpoint, CapRights::READ);
        if (!ep) return -1;

        if (rsi == INVALID_CAP_ID) {
            ep->unbind_notification();
            return 0;
        }

        Notification* notif = get_cap_typed<Notification>(
            cspace, static_cast<CapID>(rsi), ObjectType::Notification, CapRights::WRITE);
        if (!notif) return -1;
        ep->bind_notification(notif, rdx);
        return 0;
    } else if (syscall_number == 38) {
        // notification_poll(notif_cap, out_badge_ptr)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(rsi, sizeof(std::uint64_t))) return -1;
        Notification* notif = get_cap_typed<Notification>(
            cspace, static_cast<CapID>(rdi), ObjectType::Notification, CapRights::READ);
        if (!notif) return -1;
        std::uint64_t val = notif->poll();
        *reinterpret_cast<std::uint64_t*>(rsi) = val;
        return (val != 0) ? 0 : 1;
    } else if (syscall_number == 39) {
        // sys_tcb_set_exit_notification(tcb_cap, notif_cap, badge)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        TCB* tcb = get_cap_typed<TCB>(
            cspace, static_cast<CapID>(rdi), ObjectType::TCB, CapRights::WRITE);
        if (!tcb) return -1;

        if (rsi == INVALID_CAP_ID) {
            tcb->setExitNotification(nullptr, 0);
            return 0;
        }

        Notification* notif = get_cap_typed<Notification>(
            cspace, static_cast<CapID>(rsi), ObjectType::Notification, CapRights::WRITE);
        if (!notif) return -1;

        tcb->setExitNotification(notif, rdx);
        return 0;
    } else if (syscall_number == 40) {
        // sys_tcb_cancel_ipc(tcb_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        TCB* tcb = get_cap_typed<TCB>(
            cspace, static_cast<CapID>(rdi), ObjectType::TCB, CapRights::WRITE);
        if (!tcb) return -1;

        if (!tcb->cancel_ipc()) return -1;
        return 0;
    } else if (syscall_number == 41) {
        // sys_tcb_get_cpu_time(tcb_cap, out_cycles_ptr)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(rsi, sizeof(std::uint64_t))) return -1;
        if (rsi % alignof(std::uint64_t) != 0) return -1;

        ExecutionContext* target = get_cap_execution_context(
            cspace, static_cast<CapID>(rdi), CapRights::READ);
        if (!target) return -1;

        std::uint64_t cycles = target->total_cpu_cycles;
        if (target == g_get_current_context_func() && target->last_switched_in != 0) {
            cycles += (oz::x86_64::rdtsc() - target->last_switched_in);
        }
        *reinterpret_cast<std::uint64_t*>(rsi) = cycles;
        return 0;
    } else if (syscall_number == 42) {
        // sys_baton_switch(target_baton_cap, quantum_us)
        // rdi: target_baton_cap
        // rsi: quantum_us
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        ExecutionContext* current = g_get_current_context_func ? g_get_current_context_func() : nullptr;
        if (!current) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        Thread* cur_t = static_cast<Thread*>(current);
        if (cur_t->is_irq_handling()) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_STATE), 0);
        }

        Baton* baton = get_cap_typed<Baton>(
            cspace, static_cast<CapID>(rdi), ObjectType::Baton, CapRights::WRITE);
        if (!baton) {
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::ORPHANED), 0);
        }

        // 不変条件 2（デカルト木非所属不変条件）: target が irq_baton の場合は SYS_ERR_INVALID で拒絶
        if (baton->is_irq_baton) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }

        if (!baton->thread) {
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::ORPHANED), 0);
        }

        // 1. ループ検出 (デッドロック防止: 設計書 4.2 不変条件2, 5.2 (1))
        if (baton->state == BatonState::WAIT_RETURN) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_DEADLK), 0);
        }

        ExecutionContext* target = baton->thread;
        if (target->is_dead) {
            baton->thread = nullptr;
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::EXIT), 0);
        }

        // 走行中コンテキスト自身へのスイッチ禁止
        if (target == current) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_DEADLK), 0);
        }

        // 2. クォンタムの明示的注入 (設計書 5.2 (2))
        std::uint64_t quantum_us = rsi;
        if (quantum_us > 0) {
            baton->quantum = quantum_us;
            baton->remaining = quantum_us;
            baton->lazy_delta = 0;
        }

        if (!baton->has_budget()) {
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::TIMEOUT), 0);
        }

        if (target->is_blocked || target->state != ThreadState::Ready ||
            (baton->state != BatonState::WAIT_START && target->baton != baton)) {
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::BLOCKED), 0);
        }

        auto& sched = Kernel::instance().getScheduler();
        Baton* current_baton = cur_t ? cur_t->get_active_baton() : current->baton;
        if (!current_baton) current_baton = sched.running_baton;

        baton->caller_context = current;
        baton->is_active = true;

        ExecutionContext* actual_target = target;
        if (baton->state == BatonState::WAIT_START) {
            // 3. 分断バトンの自由な再開とダイレクト・フォワーディング (設計書 5.2 (3))
            Baton* true_target = baton->tail_baton ? baton->tail_baton : baton;
            actual_target = true_target->thread ? true_target->thread : target;

            baton->caller_baton = current_baton;
            if (current_baton) {
                current_baton->callee_baton = baton;
                current_baton->state = BatonState::WAIT_RETURN;
            }

            baton->state = BatonState::WAIT_RETURN;
            true_target->state = BatonState::RUNNING;

            cartesianPush(sched.cartesian_root, current_baton, baton);
            sched.cartesian_root = cartesianZipMerge(sched.cartesian_root, baton->sub_min_baton);
            baton->sub_min_baton = nullptr;

            // 真の実行末尾 true_target の祖先ノードの遅延タグを pushDown
            if (true_target != baton) {
                Baton* ancestors[32];
                int anc_count = 0;
                Baton* curr = true_target;
                while (curr && anc_count < 32) {
                    ancestors[anc_count++] = curr;
                    curr = curr->cartesian_parent;
                }
                for (int i = anc_count - 1; i >= 0; --i) {
                    cartesianPushDown(ancestors[i]);
                }
            }

            sched.running_baton = true_target;

            if (sched.cartesian_root && !sched.cartesian_root->is_infinite) {
                oz::x86_64::setLAPICOneShot(sched.cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
            } else {
                oz::x86_64::stopLAPICTimer();
            }

            current->sched_reason = SchedReason::NONE;
            std::uint64_t now = oz::x86_64::rdtsc();
            sched.performContextSwitch(current, actual_target, now);
        } else {
            // 4. 通常スイッチ (Target が READY / BLOCKED: 設計書 5.2 (4))
            baton->caller_baton = current_baton;
            if (current_baton) {
                current_baton->callee_baton = baton;
                current_baton->state = BatonState::WAIT_RETURN;
            }

            baton->state = BatonState::RUNNING;

            cartesianPush(sched.cartesian_root, current_baton, baton);
            sched.running_baton = baton;

            if (sched.cartesian_root && !sched.cartesian_root->is_infinite) {
                oz::x86_64::setLAPICOneShot(sched.cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
            } else {
                oz::x86_64::stopLAPICTimer();
            }

            current->sched_reason = SchedReason::NONE;
            std::uint64_t now = oz::x86_64::rdtsc();
            sched.performContextSwitch(current, target, now);
        }

        // ===== 復帰処理 (親スレッドのコンテキストで再開) =====
        baton->is_active = false;
        baton->caller_context = nullptr;

        std::uint64_t slice = actual_target->last_consumed_cycles;
        std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
        std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);

        SchedReason reason = current->sched_reason;
        current->sched_reason = SchedReason::NONE;

        if (baton->thread == nullptr || target->is_dead) {
            if (reason == SchedReason::NONE) {
                reason = (baton->thread == nullptr) ? SchedReason::ORPHANED : SchedReason::EXIT;
            }
        } else if (reason == SchedReason::NONE) {
            reason = target->is_blocked ? SchedReason::BLOCKED : SchedReason::YIELD;
        }

        std::uint64_t rdx_val = 0;
        if (reason == SchedReason::PREEMPTED) {
            rdx_val = current->preempted_irq_vector;
        } else {
            rdx_val = consumed_us;
        }

        return SyscallResult(static_cast<std::uint64_t>(reason), rdx_val);
    } else if (syscall_number == 43) {
        // sys_baton_inject(target_baton_cap, budget_us)
        // rdi: target_baton_cap, rsi: budget_us
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        Baton* baton = get_cap_typed<Baton>(
            cspace, static_cast<CapID>(rdi), ObjectType::Baton, CapRights::WRITE);
        if (!baton) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }

        // 走行中保護
        if (baton->state == BatonState::RUNNING) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_BUSY), 0);
        }

        // 1.3 【禁止規則】 ① チェーンに含まれるバトンへの注入禁止
        if (baton->is_active) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }

        baton->quantum = rsi;
        baton->remaining = rsi;
        baton->lazy_delta = 0;

        if (rsi > 0) {
            IRQHandler* h = baton->bound_irq_handler;
            if (!h) {
                for (int v = 0; v < 256; ++v) {
                    IRQHandler* cand = oz::irq::getHandler(v);
                    if (cand && cand->assigned_irq_baton == baton) {
                        h = cand;
                        break;
                    }
                }
            }
            if (h && h->assigned_irq_baton == baton && h->hasPending()) {
                h->clearPending();
                h->unmask();
                oz::x86_64::sendLAPICIPI(h->vector);
            }
        }

        return SyscallResult(0, 0);
    } else if (syscall_number == 44) {
        // sys_yield()
        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        if (!current) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        if (current->is_irq_handling()) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_STATE), 0);
        }

        std::uint64_t now = oz::x86_64::rdtsc();
        std::uint64_t slice = (now >= current->last_switched_in) ? (now - current->last_switched_in) : 0;
        std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
        std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
        auto& sched = Kernel::instance().getScheduler();
        Baton* b = current->get_active_baton();
        if (b && !b->is_infinite) {
            if (b->remaining > consumed_us) {
                b->remaining -= consumed_us;
            } else {
                b->remaining = 0;
            }
            cartesianPopLeaf(sched.cartesian_root, b);
            b->state = BatonState::BLOCKED;
            b->is_active = false;
        }

        current->last_consumed_cycles = slice;
        current->total_cpu_cycles += slice;
        current->last_switched_in = 0;
        current->sched_reason = SchedReason::YIELD;

        ExecutionContext* caller = nullptr;
        if (b) {
            caller = b->caller_context;
            Baton* caller_b = b->caller_baton;
            b->caller_baton = nullptr;
            if (caller_b) {
                caller_b->callee_baton = nullptr;
                caller_b->state = BatonState::RUNNING;
                cartesianPushDown(caller_b);
                sched.running_baton = caller_b;
                if (!caller) caller = caller_b->thread;
            }
            b->caller_context = nullptr;
        }

        if (caller && caller != current) {
            caller->sched_reason = SchedReason::YIELD;
            if (sched.cartesian_root && !sched.cartesian_root->is_infinite) {
                oz::x86_64::setLAPICOneShot(sched.cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
            } else {
                oz::x86_64::stopLAPICTimer();
            }
            sched.performContextSwitch(current, caller, now);
        }
        return SyscallResult(0, 0);
    } else if (syscall_number == 45) {
        // sys_tcb_get_id(tcb_cap, out_id_ptr)
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(-1, 0);
        if (!is_user_buffer(rsi, sizeof(std::uint64_t))) return SyscallResult(-1, 0);
        if (rsi % alignof(std::uint64_t) != 0) return SyscallResult(-1, 0);

        ExecutionContext* target = get_cap_execution_context(
            cspace, static_cast<CapID>(rdi), CapRights::READ);
        if (!target) return SyscallResult(-1, 0);

        *reinterpret_cast<std::uint64_t*>(rsi) = target->id;
        return SyscallResult(0, 0);
    } else if (syscall_number == 46 || syscall_number == 54) {
        // sys_baton_bind_notification(baton_cap, notif_cap, badge)
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(-1, 0);

        Notification* notif = nullptr;
        if (static_cast<CapID>(rsi) != INVALID_CAP_ID && rsi != 0) {
            notif = get_cap_typed<Notification>(
                cspace, static_cast<CapID>(rsi), ObjectType::Notification, CapRights::WRITE);
            if (!notif) return SyscallResult(-1, 0);
        }

        Baton* baton = get_cap_typed<Baton>(
            cspace, static_cast<CapID>(rdi), ObjectType::Baton, CapRights::WRITE);
        if (baton) {
            baton->bound_notif = notif;
            baton->bound_badge = rdx;
            if (baton->is_runnable()) {
                baton->notify_if_runnable();
            }
            return SyscallResult(0, 0);
        }

        TCB* tcb = get_cap_typed<TCB>(
            cspace, static_cast<CapID>(rdi), ObjectType::TCB, CapRights::WRITE);
        if (tcb && tcb->getThread()) {
            tcb->setSchedNotification(notif, rdx);
            return SyscallResult(0, 0);
        }

        return SyscallResult(-1, 0);
    } else if (syscall_number == 47) {
        // sys_irq_ack(irq_handler_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(-1, 0);

        Capability* slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!slot || slot->type() != CapType::IRQHandler || !slot->has_rights(CapRights::ACK_WAIT)) {
            return SyscallResult(-1, 0);
        }
        std::uint8_t vector = slot->irq_vector();
        IRQHandler& handler = oz::g_irq_handlers[vector];
        handler.acknowledge();
        return SyscallResult(0, 0);
    } else if (syscall_number == 48) {
        // sys_irq_handler_set_notification(irq_handler_cap, notif_cap, badge)
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(-1, 0);

        Capability* slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!slot || slot->type() != CapType::IRQHandler || !slot->has_rights(CapRights::BIND)) {
            return SyscallResult(-1, 0);
        }
        std::uint8_t vector = slot->irq_vector();
        IRQHandler* handler = &oz::g_irq_handlers[vector];

        Notification* notif = nullptr;
        if (static_cast<CapID>(rsi) != INVALID_CAP_ID && rsi != 0) {
            notif = get_cap_typed<Notification>(
                cspace, static_cast<CapID>(rsi), ObjectType::Notification, CapRights::WRITE);
            if (!notif) return SyscallResult(-1, 0);
        }

        handler->setNotification(notif, rdx);
        return SyscallResult(0, 0);
    } else if (syscall_number == 50) {
        // sys_irq_wait(irq_handler_cap, irq_baton_cap)
        // rdi: irq_handler_cap, rsi: irq_baton_cap
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        if (!current) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        Capability* slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!slot || slot->type() != CapType::IRQHandler || !slot->has_rights(CapRights::ACK_WAIT)) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }
        std::uint8_t vector = slot->irq_vector();
        IRQHandler* handler = &oz::g_irq_handlers[vector];

        Baton* next_irq_baton = get_cap_typed<Baton>(
            cspace, static_cast<CapID>(rsi), ObjectType::Baton, CapRights::USE);

        if (!handler || !next_irq_baton) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        // 1. 引数の irq_baton の妥当性検査 (不変条件 1 & 2)
        if (!next_irq_baton->is_irq_baton) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_TYPE), 0); // 通常バトンは渡せない
        }
        if (next_irq_baton->thread && next_irq_baton->thread != current) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_BUSY), 0); // 他スレッドが使用中
        }
        if (handler->handler_thread && handler->handler_thread != current) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_BUSY), 0); // ハンドラは既に別スレッドが待機中
        }

        bool was_handling_irq = false;
        Baton* prev_borrowed = current->borrowed_baton;

        // 2. 現在の借用状態の検査と消費クォンタムの減算
        if (prev_borrowed) {
            if (!prev_borrowed->is_irq_baton) {
                // 通常バトン (IPC等) を借用している状態で呼ぶのは不正
                return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_STATE), 0);
            }

            // 今回の割り込み処理で消費した実時間を算出し、借用バトンの残量 (remaining) を減算
            std::uint32_t current_ticks = oz::x86_64::getLAPICCurrentCount();
            std::uint32_t init_ticks = current->saved_frame.lapic_initial_ticks;
            std::uint64_t ticks_per_us = oz::x86_64::getLAPICTicksPerUs();
            if (ticks_per_us == 0) ticks_per_us = 1;
            std::uint64_t elapsed_us = (init_ticks >= current_ticks) ?
                ((init_ticks - current_ticks) / ticks_per_us) : prev_borrowed->remaining;

            if (prev_borrowed->remaining > elapsed_us) {
                prev_borrowed->remaining -= elapsed_us;
            } else {
                prev_borrowed->remaining = 0;
            }

            // 借用中だった irq_baton の紐づけを解除
            prev_borrowed->thread = nullptr;
            prev_borrowed->state = BatonState::READY;
            if (prev_borrowed->bound_irq_handler == handler) {
                prev_borrowed->bound_irq_handler = nullptr;
            }
            current->borrowed_baton = nullptr;
            current->baton = current->get_active_baton();
            was_handling_irq = true;
        }

        // 3. エッジケース: ハンドラ走行中に次の割り込みが到着していた場合 (Pending)
        if (was_handling_irq && handler->hasPending()) {
            handler->clearPending();
            if (next_irq_baton->remaining == 0) {
                // 予算枯渇: 即時タイムアウトとして扱い救出復帰
                Kernel::instance().getScheduler().perform_irq_timeout_unwind(current);
                return SyscallResult(0, 0);
            } else {
                // 残量あり: 即座に次回割り込み処理へ突入
                handler->mask(); // 次回処理のため Mask を維持

                current->borrowed_baton = next_irq_baton;
                current->baton = current->get_active_baton();
                next_irq_baton->thread = current;
                next_irq_baton->state = BatonState::RUNNING;

                std::uint64_t ticks_per_us = oz::x86_64::getLAPICTicksPerUs();
                if (ticks_per_us == 0) ticks_per_us = 1;
                std::uint64_t init_ticks = next_irq_baton->remaining * ticks_per_us;
                if (init_ticks > 0xFFFFFFFFULL) init_ticks = 0xFFFFFFFFULL;
                current->saved_frame.lapic_initial_ticks = static_cast<std::uint32_t>(init_ticks);
                oz::x86_64::setLAPICOneShot(next_irq_baton->remaining, oz::x86_64::APIC_TIMER_VECTOR);

                // ワンショット消費: ハンドラが実行に入るため登録を解除
                handler->handler_thread = nullptr;
                current->bound_irq_handler = nullptr;

                return SyscallResult(0, 0); // スレッドはブロックせずそのまま継続
            }
        }

        // 4. 次回新規割り込み用バトンと待機スレッドの登録 (ワンショット待機登録)
        handler->assigned_irq_baton = next_irq_baton;
        next_irq_baton->bound_irq_handler = handler;
        handler->handler_thread = current;
        current->bound_irq_handler = handler;

        // 5. 待機状態への遷移とハードウェア Unmask
        current->is_blocked = true;
        current->state = ThreadState::Blocked;
        handler->acknowledge(); // ハードウェア IRQ ラインを Unmask (有効化)

        std::uint64_t now = oz::x86_64::rdtsc();

        if (!was_handling_irq) {
            // 【初期待機／タイムアウト残余処理完了パス】
            Kernel::instance().getScheduler().yieldToSchedulerBlocked(current, now);
        } else {
            // 【割り込み完了パス】
            ExecutionContext* interrupted = current->saved_frame.interrupted_context;
            std::uint32_t rem_ticks = current->saved_frame.lapic_remaining_ticks;
            std::uint64_t entry_tsc = current->saved_frame.entry_tsc;
            current->saved_frame.clear();

            // ネストスタックのポップ
            if (g_current_irq_thread == current) {
                g_current_irq_thread = current->next_nested_irq;
                current->next_nested_irq = nullptr;
            }

            // No Time Theft 補正:
            if (interrupted) {
                Thread* int_t = static_cast<Thread*>(interrupted);
                if (!int_t->is_irq_handling()) {
                    std::uint64_t irq_duration = now - entry_tsc;
                    interrupted->last_switched_in += irq_duration;
                }
            }

            // LAPIC タイマーを復元
            if (rem_ticks > 0) {
                oz::x86_64::setLAPICRemainingTicks(rem_ticks, oz::x86_64::APIC_TIMER_VECTOR);
            } else if (interrupted) {
                Thread* int_t = static_cast<Thread*>(interrupted);
                if (int_t->get_active_baton() && !int_t->get_active_baton()->is_infinite) {
                    oz::x86_64::setLAPICRemainingTicks(1, oz::x86_64::APIC_TIMER_VECTOR);
                }
            }

            if (interrupted) {
                Kernel::instance().getScheduler().performIRQResume(interrupted, now);
            }
        }

        return SyscallResult(0, 0);
    } else if (syscall_number == 53) {
        // sys_tcb_bind_baton(tcb_cap, baton_cap)
        // rdi: tcb_cap, rsi: baton_cap
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        ExecutionContext* target = get_cap_execution_context(
            cspace, static_cast<CapID>(rdi), CapRights::WRITE);
        if (!target) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        ExecutionContext* current = g_get_current_context_func ? g_get_current_context_func() : nullptr;
        Thread* cur_t = current ? static_cast<Thread*>(current) : nullptr;
        if (cur_t && cur_t->is_irq_handling()) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_STATE), 0);
        }

        // Unbind case
        if (static_cast<CapID>(rsi) == INVALID_CAP_ID || rsi == 0) {
            Thread* t = static_cast<Thread*>(target);
            Baton* old_b = t->own_baton ? t->own_baton : t->baton;
            if (t->own_baton) {
                t->own_baton->thread = nullptr;
                t->own_baton->is_active = false;
                t->own_baton = nullptr;
            }
            t->baton = t->get_active_baton();
            if (old_b && (t->state == ThreadState::BlockedOnSend ||
                          t->state == ThreadState::BlockedOnCall ||
                          t->state == ThreadState::BlockedOnReply)) {
                t->cancel_ipc();
                t->state = ThreadState::Blocked;
                t->is_blocked = true;
            }
            if (target == current) {
                current->state = ThreadState::Blocked;
                current->is_blocked = true;
                std::uint64_t now = oz::x86_64::rdtsc();
                if (old_b && !old_b->is_infinite) {
                    std::uint64_t slice = (now >= current->last_switched_in) ? (now - current->last_switched_in) : 0;
                    std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
                    std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
                    if (old_b->remaining > consumed_us) {
                        old_b->remaining -= consumed_us;
                    } else {
                        old_b->remaining = 0;
                    }
                    cartesianPushDown(old_b);
                }
                Kernel::instance().getScheduler().yieldToSchedulerOrphaned(static_cast<Thread*>(current), old_b, now);
            }
            return SyscallResult(0, 0);
        }

        // Bind case
        Baton* new_baton = get_cap_typed<Baton>(
            cspace, static_cast<CapID>(rsi), ObjectType::Baton, CapRights::WRITE);
        if (!new_baton) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        // 不変条件 1（IRQバトンの非所有不変条件）: irq_baton を own_baton に設定することは禁止
        if (new_baton->is_irq_baton) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }

        bool current_held_baton = (current && current->baton == new_baton && new_baton->is_active);

        if (new_baton->thread && new_baton->thread != target) {
            Thread* prev_t = static_cast<Thread*>(new_baton->thread);
            if (prev_t->own_baton == new_baton) prev_t->own_baton = nullptr;
            if (prev_t->borrowed_baton == new_baton) prev_t->borrowed_baton = nullptr;
            prev_t->baton = prev_t->get_active_baton();
        }

        Thread* t = static_cast<Thread*>(target);
        if (t->own_baton && t->own_baton != new_baton) {
            t->own_baton->thread = nullptr;
            t->own_baton->is_active = false;
        }
        t->own_baton = new_baton;
        new_baton->thread = t;
        t->baton = t->get_active_baton();
        if (t->state == ThreadState::BlockedOnRecv || t->state == ThreadState::BlockedOnSend ||
            t->state == ThreadState::BlockedOnCall || t->state == ThreadState::BlockedOnReply) {
            t->cancel_ipc();
        }
        target->state = ThreadState::Ready;
        target->is_blocked = false;
        new_baton->notify_if_runnable();

        // Rebind-and-Switch: current held this baton and rebound it to another context
        if (current_held_baton && current != target) {
            Thread* cur_thread = static_cast<Thread*>(current);
            if (cur_thread->own_baton == new_baton) cur_thread->own_baton = nullptr;
            cur_thread->baton = cur_thread->get_active_baton();
            current->is_blocked = true;
            current->state = ThreadState::Blocked;
            std::uint64_t now = oz::x86_64::rdtsc();
            Kernel::instance().getScheduler().performContextSwitch(current, target, now);
        }

        return SyscallResult(0, 0);
    } else if (syscall_number == 51) {
        // sys_notification_bind_parent(child_notif_cap, parent_notif_cap, badge)
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        Notification* child = get_cap_typed<Notification>(
            cspace, static_cast<CapID>(rdi), ObjectType::Notification, CapRights::WRITE);
        if (!child) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        Notification* parent = get_cap_typed<Notification>(
            cspace, static_cast<CapID>(rsi), ObjectType::Notification, CapRights::WRITE);
        if (!parent) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        int res = child->bind_parent(parent, rdx);
        return SyscallResult(static_cast<std::uint64_t>(res), 0);
    } else if (syscall_number == 52) {
        // sys_notification_unbind_parent(child_notif_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        Notification* child = get_cap_typed<Notification>(
            cspace, static_cast<CapID>(rdi), ObjectType::Notification, CapRights::WRITE);
        if (!child) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        int res = child->unbind_parent();
        return SyscallResult(static_cast<std::uint64_t>(res), 0);
    } else if (syscall_number == 55) {
        // sys_cap_revoke(cap_id)
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        if (!cspace->revoke(static_cast<CapID>(rdi))) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }
        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        handleSelfDestructionIfNeeded(current);
        return SyscallResult(0, 0);
    } else if (syscall_number == 56) {
        // sys_baton_create_irq(irq_control_cap, untyped_cap, initial_budget_us, target_cspace, target_slot)
        // rdi: irq_control_cap, rsi: untyped_cap, rdx: initial_budget_us, r10: target_cspace, r8: target_slot
        CSpace* current_cs = get_cspace();
        if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

        CapID ctrl_cap = static_cast<CapID>(rdi);
        CapID ut_cap = static_cast<CapID>(rsi);
        std::uint64_t budget = rdx;
        CapID target_cs_id = static_cast<CapID>(r10);
        CapID target_slot = static_cast<CapID>(r8);

        if (target_slot == 0 || target_slot >= CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
        if (is_reserved_system_slot(target_slot)) return SyscallResult(SYS_ERR_INVALID, 0);

        Capability* ctrl_slot = current_cs->getSlot(ctrl_cap);
        if (!ctrl_slot || ctrl_slot->type() != CapType::IRQBatonControl) {
            return SyscallResult(SYS_ERR_PERM, 0);
        }

        Capability* ut_slot = current_cs->getSlot(ut_cap);
        if (!ut_slot || ut_slot->type() != CapType::UntypedMemory ||
            (!ut_slot->has_rights(CapRights::WRITE) && !ut_slot->has_rights(CapRights::DERIVE))) {
            return SyscallResult(SYS_ERR_PERM, 0);
        }
        if (ut_slot->untyped_is_device()) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }

        CSpace* target_cs = resolve_target_cspace(current_cs, target_cs_id);
        if (!target_cs) return SyscallResult(SYS_ERR_PERM, 0);

        Capability* slot = target_cs->getSlot(target_slot);
        if (!slot || !slot->is_null()) {
            return SyscallResult(SYS_ERR_BUSY, 0);
        }

        if (!untyped_can_allocate_bulk(ut_slot, ObjectType::Baton, sizeof(Baton), 1)) {
            return SyscallResult(SYS_ERR_NO_MEMORY, 0);
        }

        Baton* baton = untyped_allocate_object<Baton>(ut_slot, true);
        if (!baton) {
            return SyscallResult(SYS_ERR_NO_MEMORY, 0);
        }

        baton->is_irq_baton = true;
        baton->remaining = budget;
        baton->quantum = budget;
        baton->state = BatonState::READY;

        init_kernel_object_slot(slot, baton, CapRights::ALL & ~CapRights::DUPLICATE);
        cdt_insert_child(ut_slot, slot);

        return SyscallResult(0, 0);
    } else if (syscall_number == 57) {
        // sys_baton_set_budget(baton_cap, budget_us)
        // rdi: baton_cap, rsi: budget_us
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        Baton* baton = get_cap_typed<Baton>(
            cspace, static_cast<CapID>(rdi), ObjectType::Baton, CapRights::WRITE);
        if (!baton) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }

        if (!baton->is_irq_baton) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_TYPE), 0);
        }

        // 1. 走行中保護
        if (baton->state == BatonState::RUNNING) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_BUSY), 0);
        }

        // 2. 時間代入 (加算ではなく set)
        baton->remaining = rsi;
        baton->quantum = rsi;
        baton->lazy_delta = 0;

        // 3. 保留割り込みの自然なディスパッチ
        if (rsi > 0) {
            IRQHandler* h = baton->bound_irq_handler;
            if (!h) {
                for (int v = 0; v < 256; ++v) {
                    IRQHandler* cand = oz::irq::getHandler(v);
                    if (cand && cand->assigned_irq_baton == baton) {
                        h = cand;
                        break;
                    }
                }
            }
            if (h && h->assigned_irq_baton == baton && h->hasPending()) {
                h->clearPending();
                h->unmask();
                oz::x86_64::sendLAPICIPI(h->vector);
            }
        }

        return SyscallResult(0, 0);
    } else if (syscall_number == 58) {
        // sys_cap_identify(cap_id, info_ptr)
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        if (!is_user_buffer(rsi, sizeof(CapInfo))) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        Capability* slot = cspace->getSlot(static_cast<CapID>(rdi));
        if (!slot || slot->is_null()) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        CapInfo info{};
        info.type = static_cast<std::uint8_t>(slot->type());
        info.level = 0;
        info.is_mapped = slot->has_slot_flag(SLOT_FLAG_IS_MAPPED) ? 1 : 0;
        info.padding = 0;
        info.rights = static_cast<std::uint32_t>(slot->rights());
        info.size = 0;

        if (slot->type() == CapType::Frame) {
            info.size = 4096;
        } else if (slot->type() == CapType::PageTable || slot->type() == CapType::VSpace) {
            auto* pt = slot->pagetable_object();
            if (pt) info.level = pt->level();
            info.size = 4096;
        } else if (slot->type() == CapType::UntypedMemory) {
            info.size = slot->untyped_size();
        }

        utils::memcpy(reinterpret_cast<void*>(rsi), &info, sizeof(CapInfo));
        return SyscallResult(0, 0);
    } else if (syscall_number == 59) {
        // sys_untyped_retype_bulk(ut_cap, type, size_or_arg, target_cspace, start_slot, count)
        // rdi: ut_cap, rsi: type, rdx: size_or_arg, r10: target_cspace, r8: start_slot, r9: count
        return handle_untyped_retype_bulk(
            static_cast<CapID>(rdi),
            static_cast<ObjectType>(rsi),
            rdx,
            static_cast<CapID>(r10),
            static_cast<CapID>(r8),
            static_cast<std::size_t>(r9)
        );
    }
    
    return SyscallResult(static_cast<std::uint64_t>(-1), 0);
}

void initSyscall() {
    std::uint64_t efer = oz::x86_64::readMSR(oz::x86_64::MSR_EFER);
    oz::x86_64::writeMSR(oz::x86_64::MSR_EFER, efer | (1ULL << 11) | 1);

    std::uint64_t star = (static_cast<std::uint64_t>(oz::x86_64::USER_CS32) << 48) | 
                         (static_cast<std::uint64_t>(oz::x86_64::KERNEL_CS) << 32);
    oz::x86_64::writeMSR(oz::x86_64::MSR_STAR, star);

    oz::x86_64::writeMSR(oz::x86_64::MSR_LSTAR, reinterpret_cast<std::uint64_t>(syscall_entry));
    oz::x86_64::writeMSR(oz::x86_64::MSR_FMASK, 0x200);
    oz::x86_64::writeMSR(oz::x86_64::MSR_KERNEL_GS_BASE, reinterpret_cast<std::uint64_t>(&g_syscall_context));
}

void setSyscallKernelStack(std::uint64_t rsp) {
    g_syscall_context.kernel_rsp = rsp;
    oz::x86_64::writeMSR(oz::x86_64::MSR_KERNEL_GS_BASE, reinterpret_cast<std::uint64_t>(&g_syscall_context));
}

__attribute__((naked)) void syscall_entry() {
    __asm__ volatile (
        "swapgs\n"
        "movq %rsp, %gs:0\n"
        "movq %gs:8, %rsp\n"
        "pushq %gs:0\n"        // Save user RSP on thread's own kernel stack
        
        "pushq %rbp\n"

        "pushq %rcx\n"
        "pushq %r11\n"
        "pushq %r9\n"
        "pushq %r8\n"
        "pushq %r10\n"
        "pushq %rdx\n"
        "pushq %rsi\n"
        "pushq %rdi\n"
        
        "movq %rsp, %rbp\n"
        "andq $-16, %rsp\n"
        
        "movq %r10, %rcx\n"
        "pushq $0\n" 
        "pushq %rax\n"

        "call handleSyscall\n"

        // Update saved RDX slot with the 2nd return value from handleSyscall (System V ABI)
        "movq %rdx, 16(%rbp)\n"

        "movq %rbp, %rsp\n"
        
        "popq %rdi\n"
        "popq %rsi\n"
        "popq %rdx\n"
        "popq %r10\n"
        "popq %r8\n"
        "popq %r9\n"
        "popq %r11\n"
        "popq %rcx\n"
        
        "popq %rbp\n"
        "popq %rsp\n"          // Restore user RSP from kernel stack
        "swapgs\n"
        "sysretq\n"
    );
}

} // namespace oz
