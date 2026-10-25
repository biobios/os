#include "core/Syscall.hpp"
#include "hardware/x86_64.hpp"
#include "core/CapOperations.hpp"
#include "core/Thread.hpp"
#include "utils/utils.hpp"
#include "core/Frame.hpp"
#include "core/VSpace.hpp"
#include "core/UntypedMemory.hpp"
#include "core/TCB.hpp"
#include "core/CSpaceCap.hpp"
#include "core/PageTableObject.hpp"
#include "core/IRQHandler.hpp"
#include "core/IRQDispatch.hpp"
#include "core/IOPort.hpp"
#include "core/Notification.hpp"
#include "core/Baton.hpp"
#include "core/InterruptThread.hpp"

#include "core/Kernel.hpp"
#include "core/SavedInterruptFrame.hpp"

namespace oz {
constexpr int ERR_WOULDBLOCK = -2;
constexpr int ERR_CANCELLED = -3;

void (*g_exit_thread_func)() = nullptr;
CSpace* (*g_get_current_cspace_func)() = nullptr;
Thread* (*g_get_current_thread_func)() = nullptr;
ExecutionContext* (*g_get_current_context_func)() = nullptr;
void (*g_block_thread_func)() = nullptr;
void (*g_unblock_thread_func)(Thread*) = nullptr;
void (*g_queue_thread_func)(Thread* t) = nullptr;
void (*g_register_irq_handler_fn)(std::uint8_t, IRQHandler*) = nullptr;
void (*g_unregister_irq_handler_fn)(std::uint8_t, IRQHandler*) = nullptr;
void (*g_eoi_fn)() = nullptr;
std::uint8_t (*g_read_io8_fn)(std::uint16_t) = nullptr;
std::uint16_t (*g_read_io16_fn)(std::uint16_t) = nullptr;
std::uint32_t (*g_read_io32_fn)(std::uint16_t) = nullptr;
void (*g_write_io8_fn)(std::uint16_t, std::uint8_t) = nullptr;
void (*g_write_io16_fn)(std::uint16_t, std::uint16_t) = nullptr;
void (*g_write_io32_fn)(std::uint16_t, std::uint32_t) = nullptr;


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
    Capability cap = cspace->get(id);
    if (!cap.is_valid()) return nullptr;
    if ((cap.rights & required) != required) return nullptr;
    if (cap.object->objectType() != expected) return nullptr;
    return static_cast<T*>(cap.object);
}

// Capability システムにおけるポリモーフィックな型解決ヘルパー (設計書 2.1(5))
static inline ExecutionContext* get_cap_execution_context(CSpace* cspace, CapID cap_id, CapRights rights) {
    if (!cspace) return nullptr;
    Capability cap = cspace->get(cap_id);
    if (!cap.is_valid() || !cap.object) return nullptr;
    if ((cap.rights & rights) != rights) return nullptr;
    if (cap.object->objectType() == ObjectType::TCB) {
        TCB* tcb = static_cast<TCB*>(cap.object);
        return tcb->getThread();
    } else if (cap.object->objectType() == ObjectType::InterruptThread) {
        return static_cast<InterruptThread*>(cap.object);
    }
    return nullptr;
}

static inline bool configure_execution_context(ExecutionContext* ctx, CSpace* cspace, VSpace* vspace, std::uint64_t rip, std::uint64_t user_rsp) {
    if (!ctx) return false;
    ctx->cspace = cspace;
    if (vspace && vspace->getPhysicalPML4() != 0) {
        ctx->pml4_phys = VSpace::createPhysicalAddress<PageTable>(vspace->getPhysicalPML4());
    }
    ctx->user_rip = rip;
    ctx->user_rsp = user_rsp;
    ctx->state = ThreadState::Blocked;

    std::uint64_t rsp = reinterpret_cast<std::uint64_t>(ctx->stack_base) + ctx->stack_size;
    rsp &= ~15ULL;
    rsp -= sizeof(ThreadContext);
    ctx->rsp = reinterpret_cast<void*>(rsp);

    ThreadContext* tctx = reinterpret_cast<ThreadContext*>(rsp);
    tctx->rip = reinterpret_cast<std::uint64_t>(&thread_stub);
    tctx->r12 = reinterpret_cast<std::uint64_t>(&user_thread_entry);
    tctx->rbx = reinterpret_cast<std::uint64_t>(ctx);
    tctx->r13 = 0;
    tctx->r14 = 0;
    tctx->r15 = 0;
    tctx->rbp = 0;
    return true;
}

// Get current CSpace (common pattern)
static CSpace* get_cspace() {
    if (!g_get_current_cspace_func) return nullptr;
    return g_get_current_cspace_func();
}

// ===========================================================================
// ===========================================================================
// Channel IPC helpers
// ===========================================================================



static int do_channel_recv(CSpace* cspace, CapID ep_id, IPCMessage* msg, bool nonblock = false) {
    if (!cspace || !msg) return -1;
    ExecutionContext* current_ctx = g_get_current_context_func ? g_get_current_context_func() : nullptr;
    if (current_ctx && current_ctx->isInterruptThread() && !nonblock) {
        return -1; // InterruptThread はブロッキング待機禁止 (設計書 2.1(2))
    }

    ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
        cspace, ep_id, ObjectType::ChannelEndpoint, CapRights::READ);
    if (!ep) return -1;

    // 1. Check for pending asynchronous notifications (e.g. IRQ)
    std::uint64_t notif_data = 0;
    if (ep->check_and_clear_notification(notif_data)) {
        msg->data[0] = notif_data;
        msg->data[1] = 0;
        msg->data[2] = 0;
        msg->data[3] = 0;
        msg->has_cap = false;
        return 0;
    }

    ChannelEndpoint* peer = ep->get_peer();

    // 2. Synchronous rendezvous: check if a sender is already waiting on peer
    Thread* sender = peer ? peer->pop_send_waiter() : nullptr;
    if (sender) {
        *msg = sender->send_msg.user_msg;
        if (sender->send_msg.attached_cap.is_valid()) {
            CapID new_id = cspace->add(sender->send_msg.attached_cap);
            if (new_id == INVALID_CAP_ID) {
                // Failed to add to receiver CSpace: rollback to sender's CSpace!
                if (sender->cspace) {
                    sender->cspace->insert(sender->send_msg.user_msg.transferred_cap, sender->send_msg.attached_cap);
                }
                msg->has_cap = false;
            } else {
                msg->transferred_cap = new_id;
                msg->has_cap = true;
            }
            sender->send_msg.attached_cap = Capability();
        }
        sender->state = ThreadState::Ready;
        sender->is_blocked = false;
        if (g_unblock_thread_func) {
            g_unblock_thread_func(sender);
        }

        // Re-signal bound notification if more senders are waiting on peer
        if (peer->has_send_waiters() && ep->get_bound_notification()) {
            ep->get_bound_notification()->signal(ep->get_bound_badge());
        }

        return 0;
    }

    // Blocking guard for IRQ handler (Chapter 1.5 F)
    ExecutionContext* cur_ctx = g_get_current_context_func ? g_get_current_context_func() : nullptr;
    if (cur_ctx && cur_ctx->isInterruptThread()) {
        return ERR_WOULDBLOCK;
    }
    Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;

    // No sender is waiting:
    if (nonblock) {
        return ERR_WOULDBLOCK; // Would block
    }

    // Receiver must block
    if (!g_get_current_thread_func || !g_block_thread_func) return -1;
    if (!current) return -1;

    current->state = ThreadState::BlockedOnRecv;
    current->is_blocked = true;
    current->blocking_ep = ep;
    ep->add_recv_waiter(current);

    while (current->is_blocked) {
        g_block_thread_func();
        if (current->ipc_cancelled || ep->is_peer_closed()) break;
    }

    current->blocking_ep = nullptr;
    current->is_blocked = false;
    if (current->ipc_cancelled) {
        current->ipc_cancelled = false;
        return ERR_CANCELLED;
    }

    // Woken up by sender or notification
    if (ep->check_and_clear_notification(notif_data)) {
        msg->data[0] = notif_data;
        msg->data[1] = 0;
        msg->data[2] = 0;
        msg->data[3] = 0;
        msg->has_cap = false;
        return 0;
    }

    InternalIPCMessage internal;
    if (ep->pop_rendezvous_msg(internal)) {
        *msg = internal.user_msg;
        return 0;
    }

    if (ep->is_peer_closed()) return -1;
    return -1;
}

struct SyscallResult {
    std::uint64_t rax{0};
    std::uint64_t rdx{0};

    SyscallResult(std::uint64_t rax = 0, std::uint64_t rdx = 0) : rax(rax), rdx(rdx) {}
    SyscallResult(std::int64_t rax) : rax(static_cast<std::uint64_t>(rax)), rdx(0) {}
    SyscallResult(int rax) : rax(static_cast<std::uint64_t>(static_cast<std::int64_t>(rax))), rdx(0) {}
};

// ===========================================================================
// Syscall handlers
// ===========================================================================

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type-c-linkage"
extern "C" SyscallResult handleSyscall(std::uint64_t rdi, std::uint64_t rsi, std::uint64_t rdx,
                                       std::uint64_t r10, std::uint64_t r8, std::uint64_t r9,
                                       std::uint64_t syscall_number) {
#pragma GCC diagnostic pop
    (void)r9;
    if (syscall_number == 1) {
        // channel_send is removed in Baton Scheduling v2 (use sys_call / sys_reply_recv)
        return -1;
    } else if (syscall_number == 2) {
        // channel_recv
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(rsi, sizeof(IPCMessage))) return -1;
        IPCMessage* msg = reinterpret_cast<IPCMessage*>(rsi);
        bool nonblock = (rdx & 1) != 0;
        return do_channel_recv(cspace, static_cast<CapID>(rdi), msg, nonblock);
    } else if (syscall_number == 3) {
        // channel_create(untyped_cap, out0, out1)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(rsi, sizeof(CapID))) return -1;
        if (!is_user_buffer(rdx, sizeof(CapID))) return -1;
        CapID* out0 = reinterpret_cast<CapID*>(rsi);
        CapID* out1 = reinterpret_cast<CapID*>(rdx);
        if (sys_channel_create(cspace, static_cast<CapID>(rdi), out0, out1)) return 0;
        return -1;
    } else if (syscall_number == 4) {
        // vspace_map
        CSpace* cspace = get_cspace();
        if (!cspace) { dprint("[vspace_map] no cspace\n\r"); return -1; }
        
        VSpace* vspace = get_cap_typed<VSpace>(
            cspace, static_cast<CapID>(rdi), ObjectType::VSpace, CapRights::WRITE);
        if (!vspace) { dprint("[vspace_map] bad vspace cap\n\r"); return -1; }
        
        Frame* frame = get_cap_typed<Frame>(
            cspace, static_cast<CapID>(rsi), ObjectType::Frame, CapRights::READ);
        if (!frame) { dprint("[vspace_map] bad frame cap\n\r"); return -1; }
        
        std::uintptr_t virt = rdx;
        std::uintptr_t phys = frame->physicalAddress();
        std::size_t frame_size = frame->size();
        for (std::size_t offset = 0; offset < frame_size; offset += 4096) {
            if (!vspace->mapUser(virt + offset, phys + offset, r10)) {
                return -1;
            }
        }
        return 0;
    } else if (syscall_number == 5) {
        // frame_alloc
        CSpace* cspace = get_cspace();
        if (!cspace) {
            dprint("frame_alloc: no cspace\n\r");
            return -1;
        }
        
        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rdi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) {
            dprint("frame_alloc: get_cap_typed failed for untyped cap ");
            dprint_hex(rdi);
            dprint("\n\r");
            return -1;
        }
        
        Frame* frame = untyped->retypeFrame(rsi);
        if (!frame) {
            dprint("frame_alloc: untyped->retypeFrame failed! requested pages: ");
            dprint_dec(rsi);
            dprint(", remaining bytes: ");
            dprint_dec(untyped->remainingBytes());
            dprint("\n\r");
            return -1;
        }
        
        Capability frame_cap(frame, CapRights::ALL);
        CapID new_id = cspace->add(frame_cap);
        frame->release();
        
        if (new_id == INVALID_CAP_ID) {
            dprint("frame_alloc: cspace->add failed\n\r");
            return static_cast<std::uint64_t>(-1);
        }
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 60) {
        // exit
        dprint("User process exited.\n\r");
        if (g_get_current_thread_func) {
            Thread* cur = g_get_current_thread_func();
            if (cur && cur->exit_notif && !cur->exit_notif->isRevoked()) {
                std::uint64_t status = rdi & 0xFFFFFFFFULL;
                cur->exit_notif->signal(cur->exit_badge | status);
            }
        }
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
        // vspace_unmap
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        VSpace* vspace = get_cap_typed<VSpace>(
            cspace, static_cast<CapID>(rdi), ObjectType::VSpace, CapRights::WRITE);
        if (!vspace) return -1;
        
        if (!vspace->unmapUser(rsi)) return -1;
        return 0;
    } else if (syscall_number == 8) {
        // frame_phys_addr
        CSpace* cspace = get_cspace();
        if (!cspace) return 0;
        
        Frame* frame = get_cap_typed<Frame>(
            cspace, static_cast<CapID>(rdi), ObjectType::Frame, CapRights::READ);
        if (!frame) return 0;
        
        return static_cast<std::uint64_t>(frame->physicalAddress());
    } else if (syscall_number == 9) {
        // cap_mint
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        Capability src_cap = cspace->get(static_cast<CapID>(rdi));
        if (!src_cap.is_valid()) return -1;
        if (!src_cap.can_duplicate()) return -1;
        
        CapRights new_rights = static_cast<CapRights>(rsi);
        CapRights base_requested = new_rights & ~CapRights::ONCE;
        if ((src_cap.rights & base_requested) != base_requested) return -1;
        if ((new_rights & CapRights::ONCE) != CapRights::NONE) {
            if ((src_cap.rights & CapRights::WRITE) != CapRights::WRITE) return -1;
        }
        
        Capability minted(src_cap.object, new_rights);
        CapID new_id = cspace->add(minted);
        
        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 10) {
        // cap_delete
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        if (!cspace->remove(static_cast<CapID>(rdi))) return -1;
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

        if (target->isInterruptThread()) {
            auto* ithread = static_cast<InterruptThread*>(target);
            ithread->is_waiting_irq = false;
            ithread->is_interrupted = false;
        } else {
            Thread* t = static_cast<Thread*>(target);
            if (t->sched_notif && t->sched_badge != 0) {
                t->sched_notif->signal(t->sched_badge);
                return 0;
            }
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
    } else if (syscall_number == 15) {
        // cspace_create(untyped_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rdi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;
        
        CSpaceCap* cs_cap_obj = untyped->retypeCSpace();
        if (!cs_cap_obj) return -1;
        
        Capability cs_cap(cs_cap_obj, CapRights::ALL);
        CapID new_id = cspace->add(cs_cap);
        cs_cap_obj->release();
        
        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 16) {
        // cspace_cap_insert
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        CSpaceCap* target_cs = get_cap_typed<CSpaceCap>(
            cspace, static_cast<CapID>(rdi), ObjectType::CSpaceCap, CapRights::WRITE);
        if (!target_cs) return -1;
        
        Capability src_cap = cspace->get(static_cast<CapID>(rdx));
        if (!src_cap.is_valid()) return -1;
        if (!src_cap.can_duplicate()) return -1;
        
        CapRights new_rights = static_cast<CapRights>(r10);
        CapRights base_requested = new_rights & ~CapRights::ONCE;
        if ((src_cap.rights & base_requested) != base_requested) return -1;
        if ((new_rights & CapRights::ONCE) != CapRights::NONE) {
            if ((src_cap.rights & CapRights::WRITE) != CapRights::WRITE) return -1;
        }
        
        Capability minted(src_cap.object, new_rights);
        if (!target_cs->insertCap(static_cast<CapID>(rsi), minted)) return -1;
        return 0;
    } else if (syscall_number == 17) {
        // vspace_create(untyped_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rdi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;
        
        std::uintptr_t pml4_phys = 0;
        void* pml4_mem = untyped->allocateBytes(4096, 4096, pml4_phys);
        if (!pml4_mem) return -1;

        auto user_as = ProcessAddressSpace::createFromPhysical(
            VSpace::createPhysicalAddress<PageTable>(pml4_phys),
            Kernel::instance().getKernelSpace());
        if (!user_as.isValid()) return -1;
        auto* as_obj = untyped->allocateObject<ProcessAddressSpace>(std::move(user_as));
        if (!as_obj) return -1;
        
        auto* vspace = untyped->allocateObject<VSpace>(as_obj, pml4_phys);
        if (!vspace) return -1;
        Capability vs_cap(vspace, CapRights::ALL);
        CapID new_id = cspace->add(vs_cap);
        vspace->release();
        
        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 18) {
        // irq_handler_create(untyped_cap, vector, notification_ep_cap)
        CSpace* cspace = get_cspace();
        if (!cspace || !g_register_irq_handler_fn || !g_eoi_fn) return -1;
        
        std::uint8_t vector = static_cast<std::uint8_t>(rsi);
        // Whitelist check: only allow external IRQs defined in IDT (0x30 - 0x4F)
        if (vector < 0x30 || vector > 0x4F) {
            return static_cast<std::uint64_t>(-1);
        }
        // Conflict check: vector must not be already in use
        if (oz::irq::getHandler(vector) != nullptr) {
            return static_cast<std::uint64_t>(-1);
        }

        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rdi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;
        
        ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
            cspace, static_cast<CapID>(rdx), ObjectType::ChannelEndpoint, CapRights::WRITE);
        if (!ep) return -1;
        
        auto* handler = untyped->allocateObject<IRQHandler>(vector, g_register_irq_handler_fn, g_unregister_irq_handler_fn, g_eoi_fn);
        if (!handler) return -1;
        handler->setNotificationEndpoint(ep);
        handler->registerHandler();
        
        Capability irq_cap(handler, CapRights::ALL);
        CapID new_id = cspace->add(irq_cap);
        handler->release();
        
        if (new_id == INVALID_CAP_ID) {
            handler->unregisterHandler();
            return static_cast<std::uint64_t>(-1);
        }
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 19) {
        // irq_ack
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        IRQHandler* handler = get_cap_typed<IRQHandler>(
            cspace, static_cast<CapID>(rdi), ObjectType::IRQHandler, CapRights::WRITE);
        if (!handler) return -1;
        
        if (!handler->acknowledge()) return -1;
        return 0;
    } else if (syscall_number == 20) {
        // ioport_create(parent_ioport_cap, untyped_cap, base_port, port_count)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        // 1. Verify parent IOPort capability and rights
        Capability parent_cap = cspace->get(static_cast<CapID>(rdi));
        if (!parent_cap.is_valid() || !parent_cap.object || parent_cap.object->objectType() != ObjectType::IOPort) {
            return static_cast<std::uint64_t>(-1);
        }
        if ((parent_cap.rights & CapRights::WRITE) != CapRights::WRITE) {
            return static_cast<std::uint64_t>(-1);
        }
        auto* parent_ioport = static_cast<IOPort*>(parent_cap.object);

        // 2. Verify untyped memory capability
        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rsi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;

        std::uint16_t base_port = static_cast<std::uint16_t>(rdx);
        std::uint32_t port_count = static_cast<std::uint32_t>(r10);

        // 3. Check requested range is non-empty and contained within parent range
        if (port_count == 0 || !parent_ioport->contains(base_port, port_count)) {
            return static_cast<std::uint64_t>(-1);
        }
        
        auto* ioport = untyped->allocateObject<IOPort>(
            base_port,
            port_count,
            parent_ioport->read8Fn(), parent_ioport->read16Fn(), parent_ioport->read32Fn(),
            parent_ioport->write8Fn(), parent_ioport->write16Fn(), parent_ioport->write32Fn()
        );
        if (!ioport) return -1;

        parent_ioport->addChild(ioport);
        
        Capability io_cap(ioport, parent_cap.rights);
        CapID new_id = cspace->add(io_cap);
        ioport->release();
        
        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 21) {
        // ioport_read
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        IOPort* ioport = get_cap_typed<IOPort>(
            cspace, static_cast<CapID>(rdi), ObjectType::IOPort, CapRights::READ);
        if (!ioport) return -1;
        
        std::uint16_t port = static_cast<std::uint16_t>(rsi);
        if (rdx == 1) {
            std::uint8_t val;
            if (!ioport->read8(port, val)) return -1;
            return static_cast<std::uint64_t>(val);
        } else if (rdx == 2) {
            std::uint16_t val;
            if (!ioport->read16(port, val)) return -1;
            return static_cast<std::uint64_t>(val);
        } else if (rdx == 4) {
            std::uint32_t val;
            if (!ioport->read32(port, val)) return -1;
            return static_cast<std::uint64_t>(val);
        }
        return -1;
    } else if (syscall_number == 22) {
        // ioport_write
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        IOPort* ioport = get_cap_typed<IOPort>(
            cspace, static_cast<CapID>(rdi), ObjectType::IOPort, CapRights::WRITE);
        if (!ioport) return -1;
        
        std::uint16_t port = static_cast<std::uint16_t>(rsi);
        if (r10 == 1) {
            if (!ioport->write8(port, static_cast<std::uint8_t>(rdx))) return -1;
        } else if (r10 == 2) {
            if (!ioport->write16(port, static_cast<std::uint16_t>(rdx))) return -1;
        } else if (r10 == 4) {
            if (!ioport->write32(port, static_cast<std::uint32_t>(rdx))) return -1;
        } else {
            return -1;
        }
        return 0;
    } else if (syscall_number == 23) {
        // untyped_retype(untyped_cap, target_type, arg)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rdi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;

        ObjectType target_type = static_cast<ObjectType>(rsi);
        KernelObject* obj = nullptr;

        if (target_type == ObjectType::Frame) {
            obj = untyped->retypeFrame(rdx); // rdx = num_pages
        } else if (target_type == ObjectType::PageTable) {
            obj = untyped->retypePageTable();
        } else if (target_type == ObjectType::TCB) {
            obj = untyped->retypeTCB(g_queue_thread_func);
        } else if (target_type == ObjectType::InterruptThread) {
            obj = untyped->retypeInterruptThread();
        } else if (target_type == ObjectType::CSpaceCap) {
            obj = untyped->retypeCSpace();
        } else if (target_type == ObjectType::UntypedMemory) {
            obj = untyped->retypeUntyped(rdx); // rdx = size in bytes
        } else if (target_type == ObjectType::Notification) {
            obj = untyped->retypeNotification();
        } else if (target_type == ObjectType::Baton) {
            obj = untyped->retypeBaton();
        } else if (target_type == ObjectType::ChannelEndpoint) {
            ChannelEndpoint* end0 = nullptr;
            ChannelEndpoint* end1 = nullptr;
            if (!untyped->retypeChannelPair(&end0, &end1)) return -1;
            Capability cap0(end0, CapRights::ALL);
            Capability cap1(end1, CapRights::ALL);
            CapID id0 = cspace->add(cap0);
            CapID id1 = cspace->add(cap1);
            end0->release();
            end1->release();
            if (id0 == INVALID_CAP_ID || id1 == INVALID_CAP_ID) return -1;
            return (static_cast<std::uint64_t>(id1) << 32) | static_cast<std::uint64_t>(id0);
        }

        if (!obj) return -1;

        Capability cap(obj, CapRights::ALL);
        CapID new_id = cspace->add(cap);
        obj->release();

        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 24) {
        // untyped_revoke(untyped_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rdi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;

        untyped->revoke();
        return 0;
    } else if (syscall_number == 25) {
        // vspace_map_pagetable(vspace_cap, pt_cap, virt_addr, level)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        VSpace* vspace = get_cap_typed<VSpace>(
            cspace, static_cast<CapID>(rdi), ObjectType::VSpace, CapRights::WRITE);
        if (!vspace) return -1;

        PageTableObject* pt_obj = get_cap_typed<PageTableObject>(
            cspace, static_cast<CapID>(rsi), ObjectType::PageTable, CapRights::WRITE);
        if (!pt_obj) return -1;

        if (pt_obj->isMapped()) return -1;

        std::uintptr_t virt = rdx;
        std::uint8_t level = static_cast<std::uint8_t>(r10); // 1: PT, 2: PD, 3: PDPT
        if (level < 1 || level > 3) return -1;

        if (!vspace->installPageTable(virt, pt_obj->physicalAddress(), level)) {
            return -1;
        }

        pt_obj->setMapped(true, level);
        return 0;
    } else if (syscall_number == 26) {
        // frame_size(frame_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return 0;

        Frame* frame = get_cap_typed<Frame>(
            cspace, static_cast<CapID>(rdi), ObjectType::Frame, CapRights::READ);
        if (!frame) return 0;

        return static_cast<std::uint64_t>(frame->size());
    } else if (syscall_number == 27) {
        // sys_call(ep_cap, send_msg, recv_msg)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(rsi, sizeof(IPCMessage))) return -1;
        if (!is_user_buffer(rdx, sizeof(IPCMessage))) return -1;

        Capability ep_cap_check = cspace->get(static_cast<CapID>(rdi));
        if (!ep_cap_check.is_valid()) return -1;
        bool is_once = ep_cap_check.is_once();

        ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
            cspace, static_cast<CapID>(rdi), ObjectType::ChannelEndpoint, CapRights::WRITE);
        if (!ep || ep->is_peer_closed()) return -1;
        ChannelEndpoint* peer = ep->get_peer();
        if (!peer) return -1;

        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        if (!current || !current->has_baton()) return SYS_ERR_INVALID;

        const IPCMessage* msg = reinterpret_cast<const IPCMessage*>(rsi);
        InternalIPCMessage internal;
        internal.user_msg = *msg;
        if (msg->has_cap) {
            if ((ep_cap_check.rights & CapRights::GRANT) == CapRights::NONE) return -1;
            Capability src_cap = cspace->get(msg->transferred_cap);
            if (!src_cap.is_valid()) return -1;
            internal.attached_cap = src_cap;
            cspace->remove(msg->transferred_cap);
        }

        current->recv_msg_buf = reinterpret_cast<IPCMessage*>(rdx);
        current->blocking_ep = ep;

        // Check if receiver is already waiting on peer (Recv 先行 -> Call 到着)
        Thread* receiver = peer->pop_recv_waiter();
        if (receiver) {
            if (internal.attached_cap.is_valid()) {
                if (receiver->cspace) {
                    CapID new_id = receiver->cspace->add(internal.attached_cap);
                    if (new_id != INVALID_CAP_ID) {
                        internal.user_msg.transferred_cap = new_id;
                        internal.user_msg.has_cap = true;
                    }
                }
                internal.attached_cap = Capability();
            }
            receiver->kernel_ipc_msg = internal.user_msg;
            receiver->has_kernel_ipc_msg = true;
            receiver->current_reply_token = current->id;

            // Hand-off baton from client to server
            Baton* b = current->baton;
            current->baton = nullptr;
            receiver->baton = b;
            if (b) {
                b->thread = receiver;
                b->is_active = true;
            }

            current->state = ThreadState::BlockedOnReply;
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
            if (is_once) {
                cspace->remove(static_cast<CapID>(rdi));
            }
            if (current->ipc_cancelled) {
                current->ipc_cancelled = false;
                return SYS_ERR_CANCELLED;
            }
            return 0;
        }

        // Server not waiting (Call 先行 -> Recv 到着)
        ExecutionContext* caller = current->baton ? current->baton->caller_context : nullptr;
        if (!caller || caller == current) {
            // Cannot block: Root Baton / no caller context to yield to
            if (internal.attached_cap.is_valid()) {
                cspace->insert(msg->transferred_cap, internal.attached_cap);
            }
            current->blocking_ep = nullptr;
            return SYS_ERR_WOULDBLOCK;
        }

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
        if (current->baton && !current->baton->is_infinite) {
            std::uint64_t slice = (now >= current->last_switched_in) ? (now - current->last_switched_in) : 0;
            std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
            std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
            if (current->baton->budget > consumed_us) {
                current->baton->budget -= consumed_us;
            } else {
                current->baton->budget = 0;
            }
        }
        current->sched_reason = SchedReason::BLOCKED;

        current->baton->caller_context = nullptr;
        current->baton->is_active = false;
        caller->sched_reason = SchedReason::BLOCKED;
        Kernel::instance().getScheduler().performContextSwitch(current, caller, now);

        if (current->has_kernel_ipc_msg) {
            if (current->recv_msg_buf) {
                *current->recv_msg_buf = current->kernel_ipc_msg;
            }
            current->has_kernel_ipc_msg = false;
        }

        current->blocking_ep = nullptr;
        if (is_once) {
            cspace->remove(static_cast<CapID>(rdi));
        }
        if (current->ipc_cancelled) {
            current->ipc_cancelled = false;
            return SYS_ERR_CANCELLED;
        }
        return 0;
    } else if (syscall_number == 28) {
        // sys_reply_recv(reply_token, ep_cap, reply_msg, recv_msg)
        // rdi: reply_token, rsi: ep_cap, rdx: reply_msg, r10: recv_msg
        CSpace* cspace = get_cspace();
        if (!cspace) return 0;
        if (r10 != 0 && !is_user_buffer(r10, sizeof(IPCMessage))) return 0;

        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        if (!current) return 0;

        std::uint64_t reply_token = rdi;
        Baton* current_baton = current->baton;
        bool orphaned_baton = false;
        Thread* client_to_switch = nullptr;

        // Phase 1: Reply
        if (reply_token != 0) {
            Thread* client = get_thread_by_id(reply_token);
            if (client && (client->state == ThreadState::BlockedOnReply || client->state == ThreadState::BlockedOnCall)) {
                // Deliver reply
                if (rdx != 0 && is_user_buffer(rdx, sizeof(IPCMessage))) {
                    const IPCMessage* rep = reinterpret_cast<const IPCMessage*>(rdx);
                    IPCMessage reply_to_send = *rep;
                    if (rep->has_cap && client->cspace) {
                        Capability cap = cspace->get(rep->transferred_cap);
                        if (cap.is_valid()) {
                            CapID new_id = client->cspace->add(cap);
                            reply_to_send.transferred_cap = new_id;
                            reply_to_send.has_cap = (new_id != INVALID_CAP_ID);
                            cspace->remove(rep->transferred_cap);
                        }
                    }
                    client->kernel_ipc_msg = reply_to_send;
                    client->has_kernel_ipc_msg = true;
                }
                // Return baton to client
                current->baton = nullptr;
                client->baton = current_baton;
                if (current_baton) {
                    current_baton->thread = client;
                    current_baton->is_active = true;
                    current_baton->notify_if_runnable();
                }
                client->state = ThreadState::Ready;
                client->is_blocked = false;
                if (g_unblock_thread_func) g_unblock_thread_func(client);
                client_to_switch = client;
            } else {
                // Client is dead / invalid: Silent Drop & Unbind
                current->baton = nullptr;
                if (current_baton) {
                    current_baton->thread = nullptr; // Orphaned!
                    current_baton->is_active = false;
                }
                orphaned_baton = true;
            }
        } else {
            // reply_token == 0: Explicit Unbind / Transition to passive worker
            if (current_baton) {
                current->baton = nullptr;
                current_baton->thread = nullptr;
                current_baton->is_active = false;
                orphaned_baton = true;
            }
        }

        // Phase 2: Recv
        ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
            cspace, static_cast<CapID>(rsi), ObjectType::ChannelEndpoint, CapRights::READ);
        if (!ep) {
            if (client_to_switch) {
                Kernel::instance().getScheduler().performContextSwitch(current, client_to_switch, oz::x86_64::rdtsc());
            }
            return 0;
        }
        ChannelEndpoint* peer = ep->get_peer();

        // Check if a client is already waiting on peer
        Thread* sender = peer ? peer->pop_send_waiter() : nullptr;
        if (sender) {
            IPCMessage* recv_buf = reinterpret_cast<IPCMessage*>(r10);
            if (recv_buf) {
                *recv_buf = sender->send_msg.user_msg;
                if (sender->send_msg.attached_cap.is_valid()) {
                    CapID new_id = cspace->add(sender->send_msg.attached_cap);
                    recv_buf->transferred_cap = new_id;
                    recv_buf->has_cap = (new_id != INVALID_CAP_ID);
                    sender->send_msg.attached_cap = Capability();
                }
            }
            // Take sender's baton
            if (sender->baton && !current->baton) {
                current->baton = sender->baton;
                sender->baton = nullptr;
                current->baton->thread = current;
                current->baton->is_active = true;
            }
            sender->state = ThreadState::BlockedOnReply;
            sender->is_blocked = true;

            current->state = ThreadState::Ready;
            current->is_blocked = false;

            if (current->baton) {
                current->baton->notify_if_runnable();
            }

            if (orphaned_baton) {
                ExecutionContext* caller = current_baton ? current_baton->caller_context : nullptr;
                if (current_baton) {
                    current_baton->caller_context = nullptr;
                }
                if (caller && caller != current) {
                    caller->sched_reason = SchedReason::ORPHANED;
                    Kernel::instance().getScheduler().performContextSwitch(current, caller, oz::x86_64::rdtsc());
                }
            }

            return sender->id;
        }

        // No sender waiting: Server must block in BlockedOnRecv
        current->state = ThreadState::BlockedOnRecv;
        current->is_blocked = true;
        current->blocking_ep = ep;
        current->recv_msg_buf = reinterpret_cast<IPCMessage*>(r10);
        ep->add_recv_waiter(current);

        std::uint64_t now = oz::x86_64::rdtsc();
        if (orphaned_baton) {
            // SchedReason::ORPHANED back to caller
            ExecutionContext* caller = current_baton ? current_baton->caller_context : nullptr;
            if (current_baton) {
                current_baton->caller_context = nullptr;
            }
            if (caller && caller != current) {
                caller->sched_reason = SchedReason::ORPHANED;
                Kernel::instance().getScheduler().performContextSwitch(current, caller, now);
            } else if (g_block_thread_func) {
                g_block_thread_func();
            }
        } else if (client_to_switch) {
            Kernel::instance().getScheduler().performContextSwitch(current, client_to_switch, now);
        } else if (g_block_thread_func) {
            g_block_thread_func();
        }

        if (current->has_kernel_ipc_msg) {
            if (current->recv_msg_buf) {
                *current->recv_msg_buf = current->kernel_ipc_msg;
            }
            current->has_kernel_ipc_msg = false;
        }

        current->blocking_ep = nullptr;
        if (current->ipc_cancelled) {
            current->ipc_cancelled = false;
            return 0;
        }

        return current->current_reply_token;
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
    } else if (syscall_number == 33) {
        // channel_create_ext(untyped_cap, rights0, rights1, out0, out1)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(r10, sizeof(CapID))) return -1;
        if (!is_user_buffer(r8, sizeof(CapID))) return -1;
        CapRights r0 = static_cast<CapRights>(rsi);
        CapRights r1 = static_cast<CapRights>(rdx);
        CapID* out0 = reinterpret_cast<CapID*>(r10);
        CapID* out1 = reinterpret_cast<CapID*>(r8);
        if (sys_channel_create_ext(cspace, static_cast<CapID>(rdi), r0, r1, out0, out1)) return 0;
        return -1;
    } else if (syscall_number == 34) {
        // notification_create(untyped_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rdi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;
        Notification* notif = untyped->retypeNotification();
        if (!notif) return -1;
        Capability notif_cap(notif, CapRights::ALL);
        CapID new_id = cspace->add(notif_cap);
        notif->release();
        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
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

        ExecutionContext* cur_ctx = g_get_current_context_func ? g_get_current_context_func() : nullptr;
        // Blocking guard for IRQ handler (Chapter 1.5 F / Design 2.3)
        if (cur_ctx && cur_ctx->isInterruptThread()) {
            return static_cast<std::uint64_t>(SYS_ERR_WOULDBLOCK);
        }
        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        if (!current) return -1;

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

            Baton* baton = current->baton;
            if (baton && baton->caller_context) {
                __asm__ volatile("sti");
                ExecutionContext* caller = baton->caller_context;
                baton->caller_context = nullptr;
                baton->is_active = false;
                caller->sched_reason = SchedReason::BLOCKED;
                Kernel::instance().getScheduler().performContextSwitch(current, caller, oz::x86_64::rdtsc());
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
        dprint("channel_bind_notification: ep=");
        dprint_hex(reinterpret_cast<std::uint64_t>(ep));
        dprint(", notif=");
        dprint_hex(reinterpret_cast<std::uint64_t>(notif));
        dprint(", badge=");
        dprint_hex(rdx);
        dprint("\n\r");
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
        // sys_baton_switch(target_baton_cap)
        // rdi: target_baton_cap
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        ExecutionContext* current = g_get_current_context_func ? g_get_current_context_func() : nullptr;
        if (!current) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        // InterruptThread からのブロッキング操作を禁止 (設計書 2.1(2))
        if (current->isInterruptThread()) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_TYPE), 0);
        }

        Baton* baton = get_cap_typed<Baton>(
            cspace, static_cast<CapID>(rdi), ObjectType::Baton, CapRights::WRITE);
        if (!baton || !baton->thread) {
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::ORPHANED), 0);
        }

        ExecutionContext* target = baton->thread;
        if (target->is_dead) {
            baton->thread = nullptr;
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::EXIT), 0);
        }

        if (!baton->has_budget()) {
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::TIMEOUT), 0);
        }

        // 1.3 【禁止規則】 ② 走行中コンテキスト自身へのスイッチ禁止
        if (target == current) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_DEADLK), 0);
        }

        if (target->is_blocked || target->state != ThreadState::Ready) {
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::BLOCKED), 0);
        }

        // ターゲットが InterruptThread の場合、親呼出元（スケジューラ）を caller_context に設定 (設計書 4.2 C(2,3))
        if (target->isInterruptThread()) {
            auto* ithread = static_cast<InterruptThread*>(target);
            if (ithread->is_waiting_irq && !ithread->is_interrupted) {
                return SyscallResult(static_cast<std::uint64_t>(SchedReason::BLOCKED), 0);
            }
            if (ithread->is_interrupted) {
                // タイムアウト中断からの再開時のみ、呼出元（スケジューラ）を caller_context に設定
                ithread->caller_context = current;
                ithread->is_interrupted = false;
                if (ithread->irq_handler) {
                    ithread->irq_handler->clearPending();
                }
            } else {
                ithread->caller_context = nullptr;
            }
        }

        // Set one-shot LAPIC timer if finite
        if (!baton->is_infinite) {
            oz::x86_64::setLAPICOneShot(baton->budget, oz::x86_64::APIC_TIMER_VECTOR);
        }

        baton->caller_context = current;
        baton->is_active = true;

        current->sched_reason = SchedReason::NONE;
        std::uint64_t now = oz::x86_64::rdtsc();
        Kernel::instance().getScheduler().performContextSwitch(current, target, now);

        // ===== 復帰処理 (親スレッドのコンテキストで再開) =====
        oz::x86_64::stopLAPICTimer();
        baton->is_active = false;
        baton->caller_context = nullptr;

        std::uint64_t slice = target->last_consumed_cycles;
        std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
        std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
        if (!baton->is_infinite && !target->isInterruptThread()) {
            if (baton->budget > consumed_us) baton->budget -= consumed_us;
            else baton->budget = 0;
        }

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
            rdx_val = target->last_consumed_cycles;
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

        // 1.3 【禁止規則】 ① チェーンに含まれるバトンへの注入禁止
        if (baton->is_active) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }

        baton->budget = rsi;
        if (baton->thread && baton->thread->isInterruptThread()) {
            auto* ithread = static_cast<InterruptThread*>(baton->thread);
            dprint("[sys_baton_inject] InterruptThread injected budget=");
            dprint_dec(rsi);
            dprint(" is_interrupted=");
            dprint(ithread->is_interrupted ? "1" : "0");
            dprint(" is_waiting_irq=");
            dprint(ithread->is_waiting_irq ? "1" : "0");
            dprint("\n\r");
            if (ithread->is_waiting_irq && ithread->irq_handler && baton->budget > 0) {
                // 累計枯渇で保留されていた IRQ ラインを Unmask (設計書 3.3)
                ithread->irq_handler->acknowledge();
            }
        } else if (baton->is_runnable()) {
            baton->notify_if_runnable();
        }

        return SyscallResult(0, 0);
    } else if (syscall_number == 44) {
        // sys_yield()
        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        if (!current) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        std::uint64_t now = oz::x86_64::rdtsc();
        std::uint64_t slice = (now >= current->last_switched_in) ? (now - current->last_switched_in) : 0;
        std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
        std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
        if (current->baton && !current->baton->is_infinite) {
            if (current->baton->budget > consumed_us) {
                current->baton->budget -= consumed_us;
            } else {
                current->baton->budget = 0;
            }
            current->baton->is_active = false;
        }

        current->last_consumed_cycles = slice;
        current->total_cpu_cycles += slice;
        current->last_switched_in = 0;
        current->sched_reason = SchedReason::YIELD;

        ExecutionContext* caller = current->baton ? current->baton->caller_context : nullptr;
        if (caller && caller != current) {
            current->baton->caller_context = nullptr;
            caller->sched_reason = SchedReason::YIELD;
            Kernel::instance().getScheduler().performContextSwitch(current, caller, now);
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
            if (baton->thread && baton->thread->isInterruptThread()) {
                baton->is_irq_baton = true;
            }
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

        IRQHandler* handler = get_cap_typed<IRQHandler>(
            cspace, static_cast<CapID>(rdi), ObjectType::IRQHandler, CapRights::WRITE);
        if (!handler) return SyscallResult(-1, 0);

        handler->acknowledge();
        return SyscallResult(0, 0);
    } else if (syscall_number == 48) {
        // sys_irq_handler_set_notification(irq_handler_cap, notif_cap, badge)
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(-1, 0);

        IRQHandler* handler = get_cap_typed<IRQHandler>(
            cspace, static_cast<CapID>(rdi), ObjectType::IRQHandler, CapRights::WRITE);
        if (!handler) return SyscallResult(-1, 0);

        Notification* notif = nullptr;
        if (static_cast<CapID>(rsi) != INVALID_CAP_ID && rsi != 0) {
            notif = get_cap_typed<Notification>(
                cspace, static_cast<CapID>(rsi), ObjectType::Notification, CapRights::WRITE);
            if (!notif) return SyscallResult(-1, 0);
        }

        handler->setNotification(notif, rdx);
        return SyscallResult(0, 0);
    } else if (syscall_number == 49) {
        // sys_sched_set_root: Abolished in v2!
        return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
    } else if (syscall_number == 50) {
        // sys_irq_wait(irq_handler_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        ExecutionContext* current_ctx = g_get_current_context_func ? g_get_current_context_func() : nullptr;
        if (!current_ctx) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        // 呼び出し元が InterruptThread であることを厳格に検査 (設計書 2.1(2))
        if (!current_ctx->isInterruptThread()) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_TYPE), 0);
        }
        auto* current = static_cast<InterruptThread*>(current_ctx);

        IRQHandler* handler = get_cap_typed<IRQHandler>(
            cspace, static_cast<CapID>(rdi), ObjectType::IRQHandler, CapRights::WRITE);
        if (!handler) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        current->irq_handler = handler;
        current->irq_vector = handler->vector();
        handler->setHandlerThread(current);

        std::uint64_t now = oz::x86_64::rdtsc();

        // 初期待機（リザーブ）かどうかの判定：
        // saved_frame にも caller_context にも戻り先がない場合が初期待機
        if (!current->saved_frame.interrupted_context && !current->caller_context) {
            current->is_waiting_irq = true;
            current->is_interrupted = false;
            current->is_blocked = true;
            current->state = ThreadState::Blocked;
            if (current->baton) {
                current->baton->is_irq_baton = true;
                current->baton->is_active = false;
                if (current->baton->budget == 0) {
                    current->baton->budget = 10000;
                }
            }
            handler->acknowledge(); // ハードウェア IRQ ラインを Unmask (有効化)

            ExecutionContext* caller = current->baton ? current->baton->caller_context : nullptr;
            if (current->baton) current->baton->caller_context = nullptr;
            if (!caller) {
                return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
            }

            current->sched_reason = SchedReason::BLOCKED;
            caller->sched_reason = SchedReason::BLOCKED;
            Kernel::instance().getScheduler().performContextSwitch(current, caller, now);
            return SyscallResult(0, 0);
        }

        // 割り込み処理の完了（Ack & 次回待機）
        // 1. タイマ停止
        oz::x86_64::stopLAPICTimer();

        // 2. 正常完了時の消費時間計算
        std::uint64_t slice = (now >= current->last_switched_in) ? (now - current->last_switched_in) : 0;
        std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
        std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
        if (current->baton && !current->baton->is_infinite) {
            if (current->baton->budget > consumed_us) {
                current->baton->budget -= consumed_us;
            } else {
                current->baton->budget = 0;
            }
            current->baton->is_active = false;
        }

        // 3. budget の検査と IRQ ラインの Unmask/Mask 維持（Mask Invariant）
        bool has_budget = (current->baton && current->baton->budget > 0);
        dprint("[sys_irq_wait] budget_rem=");
        dprint_dec(current->baton ? current->baton->budget : 0);
        dprint(" has_pending=");
        dprint(handler->hasPending() ? "1" : "0");
        dprint(" saved_ctx=");
        dprint(current->saved_frame.interrupted_context ? "1" : "0");
        dprint("\n\r");

        if (has_budget) {
            handler->acknowledge(); // Unmask IRQ line
        } else {
            // budget == 0 の場合（累計枯渇）: Unmask は保留（Mask 維持）し、枯渇通知を発行
            dprint("[sys_irq_wait] budget == 0 (exhausted), signaling sched\n\r");
            if (current->baton && current->baton->bound_notif) {
                current->baton->bound_notif->signal(current->baton->bound_badge);
            }
        }

        // ★★★ エッジケース対応: 処理中に次の割り込みが到着していた場合 (Pending) ★★★
        if (has_budget && handler->hasPending()) {
            dprint("[sys_irq_wait] Immediate loop on pending IRQ\n\r");
            handler->clearPending();
            handler->mask(); // 次の割り込み処理のために再度 Mask

            // ハンドラを待機させず、直ちにリターンして次のイベントを即時処理させる
            current->is_waiting_irq = false;
            current->is_interrupted = false;
            current->is_blocked = false;
            current->state = ThreadState::Running;
            current->last_switched_in = now;
            return SyscallResult(0, 0);
        }

        current->is_waiting_irq = true;
        current->is_interrupted = false;
        current->is_blocked = true;
        current->state = ThreadState::Blocked;
        if (current->baton) {
            current->baton->is_irq_baton = true;
            current->baton->is_active = false;
        }

        // 4. 復帰先の動的判定
        if (current->saved_frame.interrupted_context) {
            // [ケース1: 割り込み直後の完了] -> 元スレッド T（または下位ハンドラ）へ復帰
            ExecutionContext* T = current->saved_frame.interrupted_context;
            std::uint32_t rem = current->saved_frame.lapic_remaining_ticks;
            current->saved_frame.clear();

            if (g_current_interrupt_thread == current) {
                g_current_interrupt_thread = current->next_nested;
                current->next_nested = nullptr;
            }

            if (rem > 0) {
                oz::x86_64::setLAPICRemainingTicks(rem, oz::x86_64::APIC_TIMER_VECTOR);
            }

            if (T) {
                T->last_switched_in = now;
                Kernel::instance().getScheduler().performContextSwitch(current, T, now);
            }
            return SyscallResult(0, 0);
        } else {
            // [ケース2: スケジューラからの再開後の完了] -> 呼び出し元（スケジューラ）へ通常復帰
            ExecutionContext* caller = current->caller_context;
            current->caller_context = nullptr;

            // 処理完了に伴い保留されていた IRQ ラインを Unmask
            handler->acknowledge();

            if (caller) {
                caller->sched_reason = SchedReason::NONE;
                caller->last_switched_in = now;
                Kernel::instance().getScheduler().performContextSwitch(current, caller, now);
            }
            return SyscallResult(0, 0);
        }
    } else if (syscall_number == 53) {
        // sys_tcb_bind_baton(tcb_cap, baton_cap)
        // rdi: tcb_cap, rsi: baton_cap
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        ExecutionContext* target = get_cap_execution_context(
            cspace, static_cast<CapID>(rdi), CapRights::WRITE);
        if (!target) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        ExecutionContext* current = g_get_current_context_func ? g_get_current_context_func() : nullptr;

        // Unbind case
        if (static_cast<CapID>(rsi) == INVALID_CAP_ID || rsi == 0) {
            Baton* old_b = target->baton;
            if (old_b) {
                old_b->thread = nullptr;
                old_b->is_active = false;
                target->baton = nullptr;
            }
            if (!target->isInterruptThread()) {
                Thread* t = static_cast<Thread*>(target);
                if (t->state == ThreadState::BlockedOnRecv || t->state == ThreadState::BlockedOnSend ||
                    t->state == ThreadState::BlockedOnCall || t->state == ThreadState::BlockedOnReply) {
                    t->cancel_ipc();
                    t->state = ThreadState::Blocked;
                    t->is_blocked = true;
                }
            }
            if (target == current) {
                current->state = ThreadState::Blocked;
                current->is_blocked = true;
                std::uint64_t now = oz::x86_64::rdtsc();
                ExecutionContext* caller = old_b ? old_b->caller_context : nullptr;
                if (old_b) old_b->caller_context = nullptr;
                if (caller && caller != current) {
                    caller->sched_reason = SchedReason::ORPHANED;
                    Kernel::instance().getScheduler().performContextSwitch(current, caller, now);
                }
            }
            return SyscallResult(0, 0);
        }

        // Bind case
        Baton* new_baton = get_cap_typed<Baton>(
            cspace, static_cast<CapID>(rsi), ObjectType::Baton, CapRights::WRITE);
        if (!new_baton) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        bool current_held_baton = (current && (current->baton == new_baton || new_baton->thread == current));

        if (new_baton->thread && new_baton->thread != target) {
            new_baton->thread->baton = nullptr;
        }
        if (target->baton && target->baton != new_baton) {
            target->baton->thread = nullptr;
            target->baton->is_active = false;
        }

        target->baton = new_baton;
        new_baton->thread = target;

        new_baton->is_irq_baton = target->isInterruptThread();
        if (target->isInterruptThread()) {
            auto* ithread = static_cast<InterruptThread*>(target);
            if (!ithread->is_waiting_irq) {
                target->state = ThreadState::Ready;
                target->is_blocked = false;
            }
        } else {
            Thread* t = static_cast<Thread*>(target);
            if (t->state == ThreadState::BlockedOnRecv || t->state == ThreadState::BlockedOnSend ||
                t->state == ThreadState::BlockedOnCall || t->state == ThreadState::BlockedOnReply) {
                t->cancel_ipc();
            }
            target->state = ThreadState::Ready;
            target->is_blocked = false;
        }
        new_baton->notify_if_runnable();

        // Rebind-and-Switch: current held this baton and rebound it to another context
        if (current_held_baton && current != target) {
            current->baton = nullptr;
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
