#include "core/syscall/SyscallHandlers.hpp"

namespace oz {

static SyscallResult sys_channel_create_handler(const SyscallArgs& args) {
    // channel_create(untyped_cap, target_cs0, slot0, target_cs1, slot1, modes)
    CSpace* current_cs = get_cspace();
    if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

    CapID ut_cap = static_cast<CapID>(args.rdi);
    CapID target_cs0_id = static_cast<CapID>(args.rsi);
    CapID slot0 = static_cast<CapID>(args.rdx);
    CapID target_cs1_id = static_cast<CapID>(args.r10);
    CapID slot1 = static_cast<CapID>(args.r8);
    std::uint64_t modes_raw = args.r9;

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
}

static SyscallResult sys_call_handler(const SyscallArgs& args) {
    // sys_call(ep_cap, send_msg, recv_msg, flags)
    // rdi: ep_cap, rsi: send_msg, rdx: recv_msg, r10: flags
    CSpace* cspace = get_cspace();
    if (!cspace) return SYS_ERR_INVALID;
    if (!is_user_buffer(args.rsi, sizeof(IPCMessage))) return SYS_ERR_INVALID;
    if (!is_user_buffer(args.rdx, sizeof(IPCMessage))) return SYS_ERR_INVALID;

    Capability* ep_slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!ep_slot || ep_slot->is_null()) return SYS_ERR_INVALID;

    ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::ChannelEndpoint, CapRights::WRITE);
    if (!ep || ep->is_peer_closed() || !ep->get_peer()) return SYS_ERR_INVALID;
    ChannelEndpoint* peer = ep->get_peer();

    Thread* current = get_current_thread();
    if (!current || !current->has_baton()) {
        dprint("[KERNEL PANIC] Invariant violation (C-7): sys_call invoked without active baton! tid=");
        if (current) dprint_dec(current->id);
        dprint("\n\r");
        while (true) {
            __asm__ volatile("cli; hlt");
        }
    }

    std::uint32_t flags = static_cast<std::uint32_t>(args.r10);

    // Check SEND_NONBLOCK mode
    if (ep->get_mode() == ChannelMode::SEND_NONBLOCK && !(flags & IPC_FLAG_BLOCK)) {
        if (!peer->has_recv_waiters()) {
            return SYS_ERR_WOULDBLOCK;
        }
    }

    const IPCMessage* msg = reinterpret_cast<const IPCMessage*>(args.rsi);
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

    current->recv_msg_buf = reinterpret_cast<IPCMessage*>(args.rdx);
    current->blocking_ep = ep;
    current->recv_slot = static_cast<CapID>(args.r8);

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
        unblock_thread(receiver);

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
}

static SyscallResult sys_reply_and_recv_handler(const SyscallArgs& args) {
    // sys_reply_and_recv(ep_cap, reply_msg, recv_msg, flags)
    // rdi: ep_cap, rsi: reply_msg, rdx: recv_msg, r10: flags
    CSpace* cspace = get_cspace();
    if (!cspace) return SYS_ERR_INVALID;

    Thread* current = get_current_thread();
    if (!current) return SYS_ERR_INVALID;

    CapID ep_cap_id = static_cast<CapID>(args.rdi);
    const IPCMessage* reply_msg = reinterpret_cast<const IPCMessage*>(args.rsi);
    IPCMessage* recv_msg = reinterpret_cast<IPCMessage*>(args.rdx);
    std::uint32_t flags = static_cast<std::uint32_t>(args.r10);
    CapID recv_slot = static_cast<CapID>(args.r8);

    if (args.rsi != 0 && !is_user_buffer(args.rsi, sizeof(IPCMessage))) {
        return SYS_ERR_INVALID;
    }
    if (args.rdx != 0 && !is_user_buffer(args.rdx, sizeof(IPCMessage))) {
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
        unblock_thread(client);
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
                // 返信を行っていなかった場合（own_baton で走行中）：
                // クライアントの要求はクライアントのバトンで実行されるべきなので、
                // 自身の own_baton の使用を終了し、BLOCKED でスケジューラへ戻る！
                action = NextAction::YIELD_TO_SCHEDULER_BLOCKED;
            }
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
}

static SyscallResult sys_channel_bind_notification_handler(const SyscallArgs& args) {
    // channel_bind_notification(ep_cap, notif_cap, badge)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;
    ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::ChannelEndpoint, CapRights::READ);
    if (!ep) return -1;

    if (args.rsi == INVALID_CAP_ID) {
        ep->unbind_notification();
        return 0;
    }

    Notification* notif = get_cap_typed<Notification>(
        cspace, static_cast<CapID>(args.rsi), ObjectType::Notification, CapRights::WRITE);
    if (!notif) return -1;
    ep->bind_notification(notif, args.rdx);
    return 0;
}

SyscallResult handleSyscallChannel(const SyscallArgs& args) {
    switch (args.number) {
        case Syscall::ChannelCreate:
            return sys_channel_create_handler(args);
        case Syscall::Call:
            return sys_call_handler(args);
        case Syscall::ReplyAndRecv:
            return sys_reply_and_recv_handler(args);
        case Syscall::ChannelBindNotification:
            return sys_channel_bind_notification_handler(args);
        default:
            return SyscallResult(SYS_ERR_INVALID, 0);
    }
}

} // namespace oz
