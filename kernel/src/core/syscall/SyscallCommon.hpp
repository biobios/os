#pragma once

#include <cstdint>
#include <cstddef>
#include <oz/SyscallNumbers.hpp>
#include <oz/ErrorCodes.hpp>
#include <oz/CapabilityTypes.hpp>
#include <oz/SchedTypes.hpp>

#include "core/Capability.hpp"
#include "core/CapOperations.hpp"
#include "core/Thread.hpp"
#include "core/Kernel.hpp"
#include "core/KernelUtils.hpp"
#include "core/PageTableObject.hpp"
#include "core/TCB.hpp"
#include "core/CSpaceCap.hpp"
#include "core/Channel.hpp"
#include "core/Notification.hpp"
#include "core/Baton.hpp"
#include "core/CartesianTree.hpp"
#include "core/UntypedMemory.hpp"
#include "core/IRQHandler.hpp"
#include "core/IRQDispatch.hpp"
#include "core/SavedInterruptFrame.hpp"
#include "hardware/x86_64.hpp"
#include "utils/utils.hpp"

namespace oz {

// ===========================================================================
// Syscall Data Structures
// ===========================================================================

struct SyscallResult {
    std::uint64_t rax{0};
    std::uint64_t rdx{0};

    constexpr SyscallResult(std::uint64_t rax = 0, std::uint64_t rdx = 0) : rax(rax), rdx(rdx) {}
    constexpr SyscallResult(std::int64_t rax) : rax(static_cast<std::uint64_t>(rax)), rdx(0) {}
    constexpr SyscallResult(int rax) : rax(static_cast<std::uint64_t>(static_cast<std::int64_t>(rax))), rdx(0) {}
};

struct SyscallArgs {
    std::uint64_t rdi;
    std::uint64_t rsi;
    std::uint64_t rdx;
    std::uint64_t r10;
    std::uint64_t r8;
    std::uint64_t r9;
    Syscall number;
};

// ===========================================================================
// Phase 5: Security & Pointer validation helpers
// ===========================================================================

inline constexpr std::uintptr_t USER_SPACE_LIMIT = 0x0000800000000000ULL;

inline bool is_user_address(std::uintptr_t addr) {
    return addr != 0 && addr < USER_SPACE_LIMIT;
}

inline bool is_user_buffer(std::uintptr_t addr, std::size_t size) {
    if (addr == 0 || size == 0) return false;
    if (addr >= USER_SPACE_LIMIT) return false;
    if (addr + size < addr) return false;  // overflow check
    if (addr + size > USER_SPACE_LIMIT) return false;
    return true;
}

// ===========================================================================
// Capability Resolution & Type Traits (Refactoring Proposal 5)
// ===========================================================================

template <typename T>
struct CapTraits;

#define OZ_DECLARE_CAP_TRAITS(Type, EnumVal) \
template <> struct CapTraits<Type> { static constexpr CapType type = EnumVal; }

OZ_DECLARE_CAP_TRAITS(UntypedMemory, CapType::UntypedMemory);
OZ_DECLARE_CAP_TRAITS(ChannelEndpoint, CapType::ChannelEndpoint);
OZ_DECLARE_CAP_TRAITS(Notification, CapType::Notification);
OZ_DECLARE_CAP_TRAITS(TCB, CapType::TCB);
OZ_DECLARE_CAP_TRAITS(CSpaceCap, CapType::CSpaceCap);
OZ_DECLARE_CAP_TRAITS(Baton, CapType::Baton);
OZ_DECLARE_CAP_TRAITS(PageTableObject, CapType::PageTable);
OZ_DECLARE_CAP_TRAITS(IRQHandler, CapType::IRQHandler);

#undef OZ_DECLARE_CAP_TRAITS

template <typename T>
struct CapResult {
    T* value = nullptr;
    std::int64_t error = 0; // SYS_ERR_* or 0 on success

    constexpr bool ok() const { return value != nullptr && error == 0; }
    constexpr explicit operator bool() const { return ok(); }
    constexpr T* operator->() const { return value; }
    constexpr T& operator*() const { return *value; }
    constexpr T* get() const { return value; }
};

template <typename T>
inline CapResult<T> resolve_cap(CSpace* cspace, CapID id, CapRights required = CapRights::NONE) {
    if (!cspace) return {nullptr, SYS_ERR_INVALID};
    if (id >= CSpace::MAX_CAPS) return {nullptr, SYS_ERR_INVALID};
    Capability* slot = cspace->getSlot(id);
    if (!slot || slot->is_null() || !slot->object()) return {nullptr, SYS_ERR_INVALID};
    if (slot->type() != CapTraits<T>::type) return {nullptr, SYS_ERR_INVALID};
    if (!slot->has_rights(required)) return {nullptr, SYS_ERR_PERM};
    return {static_cast<T*>(slot->object()), 0};
}

template <typename T>
inline T* get_cap_typed(CSpace* cspace, CapID id, CapRights required = CapRights::NONE) {
    auto res = resolve_cap<T>(cspace, id, required);
    return res.value;
}

template <typename T>
inline T* get_cap_typed(CSpace* cspace, CapID id, ObjectType expected, CapRights required) {
    if (!cspace || id >= CSpace::MAX_CAPS) return nullptr;
    Capability* slot = cspace->getSlot(id);
    if (!slot || slot->is_null() || !slot->object()) return nullptr;
    if (slot->type() != expected) return nullptr;
    if (!slot->has_rights(required)) return nullptr;
    return static_cast<T*>(slot->object());
}

inline ExecutionContext* get_cap_execution_context(CSpace* cspace, CapID cap_id, CapRights rights) {
    if (!cspace || cap_id >= CSpace::MAX_CAPS) return nullptr;
    Capability* slot = cspace->getSlot(cap_id);
    if (!slot || slot->is_null() || !slot->object()) return nullptr;
    if (!slot->has_rights(rights)) return nullptr;
    if (slot->type() == CapType::TCB) {
        TCB* tcb = static_cast<TCB*>(slot->object());
        return tcb->getThread();
    }
    return nullptr;
}

inline CSpace* get_cspace() {
    return get_current_cspace();
}

inline CSpace* resolve_target_cspace(CSpace* current_cs, CapID target_cs_id, CapRights rights = CapRights::WRITE) {
    if (!current_cs) return nullptr;
    if (target_cs_id == 0 || target_cs_id == CAP_SELF_CSPACE) {
        return current_cs;
    }
    CSpaceCap* cs_cap = get_cap_typed<CSpaceCap>(current_cs, target_cs_id, ObjectType::CSpaceCap, rights);
    if (!cs_cap) return nullptr;
    return cs_cap->getCSpace();
}

// ===========================================================================
// IPC Capability Transfer / Drop
// ===========================================================================

inline void transfer_or_drop_ipc_cap(
    CSpace* dst_cs, CapID dst_slot,
    CSpace* src_cs, CapID src_id,
    bool& out_has_cap, CapID& out_cap_id
) {
    out_has_cap = false;
    out_cap_id = 0;
    if (!src_cs || !dst_cs) return;

    if (src_id == 0 || src_id >= CSpace::MAX_CAPS || is_reserved_system_slot(src_id)) {
        return;
    }
    Capability* src = src_cs->getSlot(src_id);
    if (!src || src->is_null() || !src->has_rights(CapRights::TRANSFER)) {
        return;
    }

    bool dst_slot_valid = (dst_slot != 0 && dst_slot < CSpace::MAX_CAPS &&
                           !is_reserved_system_slot(dst_slot));
    Capability* dest = dst_slot_valid ? dst_cs->getSlot(dst_slot) : nullptr;

    if (dest && dest->is_null()) {
        if (cdt_move_slot(dest, src)) {
            out_has_cap = true;
            out_cap_id = dst_slot;
            return;
        }
    }

    src_cs->remove(src_id);
}

// ===========================================================================
// Self Destruction Handling
// ===========================================================================

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

// ===========================================================================
// Untyped Allocation Simulation
// ===========================================================================

inline bool untyped_can_allocate_bulk(
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

// ===========================================================================
// Scope TLB Flusher
// ===========================================================================

struct ScopeTlbFlusher {
    ~ScopeTlbFlusher() {
        flush_tlb_if_pending();
    }
};

} // namespace oz
