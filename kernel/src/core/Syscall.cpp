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
#include "core/CartesianTree.hpp"
#include "core/IRQBatonControl.hpp"

#include "core/Kernel.hpp"
#include "core/SavedInterruptFrame.hpp"

namespace oz {
constexpr int ERR_WOULDBLOCK = -2;
constexpr int ERR_CANCELLED = -3;
constexpr int SYS_ERR_INVALID_STATE = -7;
constexpr int SYS_ERR_PERM          = -8;

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
    Thread* current_t = current_ctx ? static_cast<Thread*>(current_ctx) : nullptr;
    if (current_t && current_t->is_irq_handling() && !nonblock) {
        return SYS_ERR_INVALID_STATE;
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

    Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;

    // No sender is waiting:
    bool effective_nonblock = nonblock || (ep->get_mode() == ChannelMode::RECV_NONBLOCK);
    if (effective_nonblock) {
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
        // channel_create(untyped_cap, out0, out1, mode0, mode1)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(rsi, sizeof(CapID))) return -1;
        if (!is_user_buffer(rdx, sizeof(CapID))) return -1;
        CapID* out0 = reinterpret_cast<CapID*>(rsi);
        CapID* out1 = reinterpret_cast<CapID*>(rdx);
        ChannelMode mode0 = static_cast<ChannelMode>(r10);
        ChannelMode mode1 = static_cast<ChannelMode>(r8);
        if (sys_channel_create(cspace, static_cast<CapID>(rdi), out0, out1, mode0, mode1)) return 0;
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
        
        std::uint64_t badge = (src_cap.badge != 0) ? src_cap.badge : rdx;
        Capability minted(src_cap.object, new_rights, badge);
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
        
        Capability minted(src_cap.object, new_rights, src_cap.badge);
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
        // sys_call(ep_cap, send_msg, recv_msg, flags)
        // rdi: ep_cap, rsi: send_msg, rdx: recv_msg, r10: flags
        CSpace* cspace = get_cspace();
        if (!cspace) return SYS_ERR_INVALID;
        if (!is_user_buffer(rsi, sizeof(IPCMessage))) return SYS_ERR_INVALID;
        if (!is_user_buffer(rdx, sizeof(IPCMessage))) return SYS_ERR_INVALID;

        Capability ep_cap_check = cspace->get(static_cast<CapID>(rdi));
        if (!ep_cap_check.is_valid()) return SYS_ERR_INVALID;
        bool is_once = ep_cap_check.is_once();

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
                if (is_once) cspace->remove(static_cast<CapID>(rdi));
                return SYS_ERR_WOULDBLOCK;
            }
        }

        const IPCMessage* msg = reinterpret_cast<const IPCMessage*>(rsi);
        InternalIPCMessage internal;
        internal.user_msg = *msg;
        internal.user_msg.badge = ep_cap_check.badge;
        if (msg->has_cap) {
            if ((ep_cap_check.rights & CapRights::GRANT) == CapRights::NONE) return SYS_ERR_INVALID;
            Capability src_cap = cspace->get(msg->transferred_cap);
            if (!src_cap.is_valid()) return SYS_ERR_INVALID;
            internal.attached_cap = src_cap;
            cspace->remove(msg->transferred_cap);
        }

        current->recv_msg_buf = reinterpret_cast<IPCMessage*>(rdx);
        current->blocking_ep = ep;

        // Baton to lend is current thread's active baton
        Baton* baton_to_lend = current->get_active_baton();

        // Check if receiver is already waiting on peer (Recv 先行 -> Call 到着)
        Thread* receiver = peer->pop_recv_waiter();
        if (receiver) {
            dprint("[sys_call] Receiver found! Handoff baton and direct switch\n\r");
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
        dprint("[sys_call] Server not waiting! Added send waiter, yielding BLOCKED\n\r");
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
        if (is_once) {
            cspace->remove(static_cast<CapID>(rdi));
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
                IPCMessage reply_to_send = *reply_msg;
                if (reply_msg->has_cap && client->cspace) {
                    Capability cap = cspace->get(reply_msg->transferred_cap);
                    if (cap.is_valid()) {
                        CapID new_id = client->cspace->add(cap);
                        reply_to_send.transferred_cap = new_id;
                        reply_to_send.has_cap = (new_id != INVALID_CAP_ID);
                        cspace->remove(reply_msg->transferred_cap);
                    } else {
                        reply_to_send.has_cap = false;
                    }
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
                if (sender->send_msg.attached_cap.is_valid()) {
                    CapID new_id = cspace->add(sender->send_msg.attached_cap);
                    recv_msg->transferred_cap = new_id;
                    recv_msg->has_cap = (new_id != INVALID_CAP_ID);
                    sender->send_msg.attached_cap = Capability();
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
    } else if (syscall_number == 33) {
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(r10, sizeof(CapID))) return -1;
        if (!is_user_buffer(r8, sizeof(CapID))) return -1;
        CapRights r0 = static_cast<CapRights>(rsi);
        CapRights r1 = static_cast<CapRights>(rdx);
        CapID* out0 = reinterpret_cast<CapID*>(r10);
        CapID* out1 = reinterpret_cast<CapID*>(r8);
        ChannelMode m0 = static_cast<ChannelMode>(r9 & 0xFFFFFFFF);
        ChannelMode m1 = static_cast<ChannelMode>((r9 >> 32) & 0xFFFFFFFF);
        if (sys_channel_create_ext(cspace, static_cast<CapID>(rdi), r0, r1, out0, out1, m0, m1)) return 0;
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
        // sys_baton_switch(target_baton_cap, quantum_us)
        // rdi: target_baton_cap
        // rsi: quantum_us
        dprint("[DEBUG sys_baton_switch ENTER] cap=");
        dprint_dec(rdi);
        dprint(" q=");
        dprint_dec(rsi);
        dprint("\n\r");
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
            dprint("[DEBUG sys_baton_switch FAIL] !baton\n\r");
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::ORPHANED), 0);
        }

        // 不変条件 2（デカルト木非所属不変条件）: target が irq_baton の場合は SYS_ERR_INVALID で拒絶
        if (baton->is_irq_baton) {
            dprint("[DEBUG sys_baton_switch FAIL] is_irq_baton\n\r");
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }

        if (!baton->thread) {
            dprint("[DEBUG sys_baton_switch FAIL] !baton->thread\n\r");
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::ORPHANED), 0);
        }

        // 1. ループ検出 (デッドロック防止: 設計書 4.2 不変条件2, 5.2 (1))
        if (baton->state == BatonState::WAIT_RETURN) {
            dprint("[DEBUG sys_baton_switch FAIL] WAIT_RETURN\n\r");
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_DEADLK), 0);
        }

        ExecutionContext* target = baton->thread;
        if (target->is_dead) {
            dprint("[DEBUG sys_baton_switch FAIL] target is_dead\n\r");
            baton->thread = nullptr;
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::EXIT), 0);
        }

        // 走行中コンテキスト自身へのスイッチ禁止
        if (target == current) {
            dprint("[DEBUG sys_baton_switch FAIL] target == current\n\r");
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
            dprint("[DEBUG sys_baton_switch FAIL] !has_budget\n\r");
            return SyscallResult(static_cast<std::uint64_t>(SchedReason::TIMEOUT), 0);
        }

        if (target->is_blocked || target->state != ThreadState::Ready ||
            (baton->state != BatonState::WAIT_START && target->baton != baton)) {
            dprint("[DEBUG sys_baton_switch FAIL] target is_blocked or not Ready state=");
            dprint_dec(static_cast<std::uint64_t>(target->state));
            dprint(" is_blocked=");
            dprint_dec(target->is_blocked ? 1 : 0);
            dprint("\n\r");
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
        // sys_irq_wait(irq_handler_cap, irq_baton_cap)
        // rdi: irq_handler_cap, rsi: irq_baton_cap
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        Thread* current = g_get_current_thread_func ? g_get_current_thread_func() : nullptr;
        if (!current) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        IRQHandler* handler = get_cap_typed<IRQHandler>(
            cspace, static_cast<CapID>(rdi), ObjectType::IRQHandler, CapRights::WRITE);
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
        return SyscallResult(0, 0);
    } else if (syscall_number == 56) {
        // sys_baton_create_irq(irq_control_cap, untyped_cap, initial_budget_us)
        // rdi: irq_control_cap, rsi: untyped_cap, rdx: initial_budget_us
        CSpace* cspace = get_cspace();
        if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

        IRQBatonControl* ctrl = get_cap_typed<IRQBatonControl>(
            cspace, static_cast<CapID>(rdi), ObjectType::IRQBatonControl, CapRights::WRITE);
        if (!ctrl) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_PERM), 0);
        }

        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rsi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }

        Baton* baton = untyped->allocateObject<Baton>(true);
        if (!baton) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }

        baton->is_irq_baton = true;
        baton->remaining = rdx;
        baton->quantum = rdx;
        baton->state = BatonState::READY;

        Capability new_cap(baton, CapRights::ALL);
        CapID new_slot = cspace->add(new_cap);
        baton->release();
        if (new_slot == INVALID_CAP_ID) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
        }

        return SyscallResult(new_slot, 0);
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
