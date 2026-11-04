#include "core/syscall/SyscallHandlers.hpp"

namespace oz {

static SyscallResult sys_exit_handler(const SyscallArgs& args) {
    // exit
    Thread* cur = get_current_thread();
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
            std::uint64_t status = args.rdi & 0xFFFFFFFFULL;
            cur->exit_notif->signal(cur->exit_badge | status);
        }
    }
    dprint("User process exited.\n\r");
    exit_current_thread();
    return 0;
}

static SyscallResult sys_tcb_configure_handler(const SyscallArgs& args) {
    // tcb_configure(tcb_cap, cspace_cap, vspace_cap, rip, user_rsp)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    ExecutionContext* target = get_cap_execution_context(
        cspace, static_cast<CapID>(args.rdi), CapRights::WRITE);
    if (!target) return -1;

    CSpaceCap* target_cspace = get_cap_typed<CSpaceCap>(
        cspace, static_cast<CapID>(args.rsi), ObjectType::CSpaceCap, CapRights::READ);
    if (!target_cspace) return -1;

    VSpace* target_vspace = get_cap_typed<VSpace>(
        cspace, static_cast<CapID>(args.rdx), ObjectType::VSpace, CapRights::READ);
    if (!target_vspace) return -1;

    if (!is_user_address(args.r10)) return -1; // rip
    if (!is_user_address(args.r8)) return -1;  // user_rsp

    if (!configure_execution_context(target, target_cspace->getCSpace(), target_vspace, args.r10, args.r8)) {
        return -1;
    }
    return 0;
}

static SyscallResult sys_tcb_resume_handler(const SyscallArgs& args) {
    // tcb_resume
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    ExecutionContext* target = get_cap_execution_context(
        cspace, static_cast<CapID>(args.rdi), CapRights::WRITE);
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
}

static SyscallResult sys_tcb_suspend_handler(const SyscallArgs& args) {
    // tcb_suspend
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    ExecutionContext* target = get_cap_execution_context(
        cspace, static_cast<CapID>(args.rdi), CapRights::WRITE);
    if (!target) return -1;

    target->is_blocked = true;
    return 0;
}

static SyscallResult sys_tcb_set_fault_endpoint_handler(const SyscallArgs& args) {
    // sys_tcb_set_fault_endpoint(tcb_cap, ep_cap)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    TCB* tcb = get_cap_typed<TCB>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::TCB, CapRights::WRITE);
    if (!tcb) return -1;

    ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
        cspace, static_cast<CapID>(args.rsi), ObjectType::ChannelEndpoint, CapRights::WRITE | CapRights::READ);
    if (!ep) return -1;

    tcb->setFaultEndpoint(ep);
    return 0;
}

static SyscallResult sys_notification_signal_handler(const SyscallArgs& args) {
    // notification_signal(notif_cap, badge)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;
    Notification* notif = get_cap_typed<Notification>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::Notification, CapRights::WRITE);
    if (!notif) return -1;
    notif->signal(args.rsi);
    return 0;
}

static SyscallResult sys_notification_wait_handler(const SyscallArgs& args) {
    // notification_wait(notif_cap, out_badge_ptr)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;
    if (!is_user_buffer(args.rsi, sizeof(std::uint64_t))) return -1;
    Notification* notif = get_cap_typed<Notification>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::Notification, CapRights::READ);
    if (!notif) return -1;

    Thread* current = get_current_thread();
    if (!current) return -1;
    if (current->is_irq_handling()) {
        return static_cast<std::uint64_t>(SYS_ERR_INVALID_STATE);
    }

    while (true) {
        __asm__ volatile("cli");
        if (notif->has_signals()) {
            __asm__ volatile("sti");
            current->is_blocked = false;
            *reinterpret_cast<std::uint64_t*>(args.rsi) = notif->get_and_clear_value();
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
        } else {
            current->sched_reason = SchedReason::BLOCKED;
            block_current_thread();
        }
    }
}

static SyscallResult sys_notification_poll_handler(const SyscallArgs& args) {
    // notification_poll(notif_cap, out_badge_ptr)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;
    if (!is_user_buffer(args.rsi, sizeof(std::uint64_t))) return -1;
    Notification* notif = get_cap_typed<Notification>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::Notification, CapRights::READ);
    if (!notif) return -1;
    std::uint64_t val = notif->poll();
    *reinterpret_cast<std::uint64_t*>(args.rsi) = val;
    return (val != 0) ? 0 : 1;
}

static SyscallResult sys_tcb_set_exit_notification_handler(const SyscallArgs& args) {
    // sys_tcb_set_exit_notification(tcb_cap, notif_cap, badge)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    TCB* tcb = get_cap_typed<TCB>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::TCB, CapRights::WRITE);
    if (!tcb) return -1;

    if (args.rsi == INVALID_CAP_ID) {
        tcb->setExitNotification(nullptr, 0);
        return 0;
    }

    Notification* notif = get_cap_typed<Notification>(
        cspace, static_cast<CapID>(args.rsi), ObjectType::Notification, CapRights::WRITE);
    if (!notif) return -1;

    tcb->setExitNotification(notif, args.rdx);
    return 0;
}

static SyscallResult sys_tcb_cancel_ipc_handler(const SyscallArgs& args) {
    // sys_tcb_cancel_ipc(tcb_cap)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    TCB* tcb = get_cap_typed<TCB>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::TCB, CapRights::WRITE);
    if (!tcb) return -1;

    if (!tcb->cancel_ipc()) return -1;
    return 0;
}

static SyscallResult sys_tcb_get_cpu_time_handler(const SyscallArgs& args) {
    // sys_tcb_get_cpu_time(tcb_cap, out_cycles_ptr)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;
    if (!is_user_buffer(args.rsi, sizeof(std::uint64_t))) return -1;
    if (args.rsi % alignof(std::uint64_t) != 0) return -1;

    ExecutionContext* target = get_cap_execution_context(
        cspace, static_cast<CapID>(args.rdi), CapRights::READ);
    if (!target) return -1;

    std::uint64_t cycles = target->total_cpu_cycles;
    if (target == get_current_context() && target->last_switched_in != 0) {
        cycles += (oz::x86_64::rdtsc() - target->last_switched_in);
    }
    *reinterpret_cast<std::uint64_t*>(args.rsi) = cycles;
    return 0;
}

static SyscallResult sys_baton_switch_handler(const SyscallArgs& args) {
    // sys_baton_switch(target_baton_cap, quantum_us)
    // rdi: target_baton_cap, rsi: quantum_us
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    ExecutionContext* current = get_current_context();
    if (!current) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    Thread* cur_t = static_cast<Thread*>(current);
    if (cur_t->is_irq_handling()) {
        return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_STATE), 0);
    }

    Baton* baton = get_cap_typed<Baton>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::Baton, CapRights::WRITE);
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
    std::uint64_t quantum_us = args.rsi;
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
}

static SyscallResult sys_baton_inject_handler(const SyscallArgs& args) {
    // sys_baton_inject(target_baton_cap, budget_us)
    // rdi: target_baton_cap, rsi: budget_us
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    Baton* baton = get_cap_typed<Baton>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::Baton, CapRights::WRITE);
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

    baton->quantum = args.rsi;
    baton->remaining = args.rsi;
    baton->lazy_delta = 0;

    if (args.rsi > 0) {
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

static SyscallResult sys_yield_handler(const SyscallArgs& args) {
    (void)args;
    // sys_yield()
    Thread* current = get_current_thread();
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
}

static SyscallResult sys_tcb_get_id_handler(const SyscallArgs& args) {
    // sys_tcb_get_id(tcb_cap, out_id_ptr)
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(-1, 0);
    if (!is_user_buffer(args.rsi, sizeof(std::uint64_t))) return SyscallResult(-1, 0);
    if (args.rsi % alignof(std::uint64_t) != 0) return SyscallResult(-1, 0);

    ExecutionContext* target = get_cap_execution_context(
        cspace, static_cast<CapID>(args.rdi), CapRights::READ);
    if (!target) return SyscallResult(-1, 0);

    *reinterpret_cast<std::uint64_t*>(args.rsi) = target->id;
    return SyscallResult(0, 0);
}

static SyscallResult sys_baton_bind_notification_handler(const SyscallArgs& args) {
    // sys_baton_bind_notification(baton_cap, notif_cap, badge)
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(-1, 0);

    Notification* notif = nullptr;
    if (static_cast<CapID>(args.rsi) != INVALID_CAP_ID && args.rsi != 0) {
        notif = get_cap_typed<Notification>(
            cspace, static_cast<CapID>(args.rsi), ObjectType::Notification, CapRights::WRITE);
        if (!notif) return SyscallResult(-1, 0);
    }

    Baton* baton = get_cap_typed<Baton>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::Baton, CapRights::WRITE);
    if (baton) {
        baton->bound_notif = notif;
        baton->bound_badge = args.rdx;
        if (baton->is_runnable()) {
            baton->notify_if_runnable();
        }
        return SyscallResult(0, 0);
    }

    TCB* tcb = get_cap_typed<TCB>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::TCB, CapRights::WRITE);
    if (tcb && tcb->getThread()) {
        tcb->setSchedNotification(notif, args.rdx);
        return SyscallResult(0, 0);
    }

    return SyscallResult(-1, 0);
}

static SyscallResult sys_tcb_bind_baton_handler(const SyscallArgs& args) {
    // sys_tcb_bind_baton(tcb_cap, baton_cap)
    // rdi: tcb_cap, rsi: baton_cap
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    ExecutionContext* target = get_cap_execution_context(
        cspace, static_cast<CapID>(args.rdi), CapRights::WRITE);
    if (!target) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    ExecutionContext* current = get_current_context();
    Thread* cur_t = current ? static_cast<Thread*>(current) : nullptr;
    if (cur_t && cur_t->is_irq_handling()) {
        return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_STATE), 0);
    }

    // Unbind case
    if (static_cast<CapID>(args.rsi) == INVALID_CAP_ID || args.rsi == 0) {
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
        cspace, static_cast<CapID>(args.rsi), ObjectType::Baton, CapRights::WRITE);
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
}

static SyscallResult sys_notification_bind_parent_handler(const SyscallArgs& args) {
    // sys_notification_bind_parent(child_notif_cap, parent_notif_cap, badge)
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    Notification* child = get_cap_typed<Notification>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::Notification, CapRights::WRITE);
    if (!child) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    Notification* parent = get_cap_typed<Notification>(
        cspace, static_cast<CapID>(args.rsi), ObjectType::Notification, CapRights::WRITE);
    if (!parent) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    int res = child->bind_parent(parent, args.rdx);
    return SyscallResult(static_cast<std::uint64_t>(res), 0);
}

static SyscallResult sys_notification_unbind_parent_handler(const SyscallArgs& args) {
    // sys_notification_unbind_parent(child_notif_cap)
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    Notification* child = get_cap_typed<Notification>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::Notification, CapRights::WRITE);
    if (!child) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    int res = child->unbind_parent();
    return SyscallResult(static_cast<std::uint64_t>(res), 0);
}

static SyscallResult sys_baton_create_irq_handler(const SyscallArgs& args) {
    // sys_baton_create_irq(irq_control_cap, untyped_cap, initial_budget_us, target_cspace, target_slot)
    // rdi: irq_control_cap, rsi: untyped_cap, rdx: initial_budget_us, r10: target_cspace, r8: target_slot
    CSpace* current_cs = get_cspace();
    if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

    CapID ctrl_cap = static_cast<CapID>(args.rdi);
    CapID ut_cap = static_cast<CapID>(args.rsi);
    std::uint64_t budget = args.rdx;
    CapID target_cs_id = static_cast<CapID>(args.r10);
    CapID target_slot = static_cast<CapID>(args.r8);

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
}

static SyscallResult sys_baton_set_budget_handler(const SyscallArgs& args) {
    // sys_baton_set_budget(baton_cap, budget_us)
    // rdi: baton_cap, rsi: budget_us
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    Baton* baton = get_cap_typed<Baton>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::Baton, CapRights::WRITE);
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
    baton->remaining = args.rsi;
    baton->quantum = args.rsi;
    baton->lazy_delta = 0;

    // 3. 保留割り込みの自然なディスパッチ
    if (args.rsi > 0) {
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

SyscallResult handleSyscallProcess(const SyscallArgs& args) {
    switch (args.number) {
        case Syscall::Exit:
            return sys_exit_handler(args);
        case Syscall::TcbConfigure:
            return sys_tcb_configure_handler(args);
        case Syscall::TcbResume:
            return sys_tcb_resume_handler(args);
        case Syscall::TcbSuspend:
            return sys_tcb_suspend_handler(args);
        case Syscall::TcbSetFaultEndpoint:
            return sys_tcb_set_fault_endpoint_handler(args);
        case Syscall::NotificationSignal:
            return sys_notification_signal_handler(args);
        case Syscall::NotificationWait:
            return sys_notification_wait_handler(args);
        case Syscall::NotificationPoll:
            return sys_notification_poll_handler(args);
        case Syscall::TcbBindNotification:
            return sys_tcb_set_exit_notification_handler(args);
        case Syscall::TcbGetBudget: // syscall 40: cancel_ipc
            return sys_tcb_cancel_ipc_handler(args);
        case Syscall::TcbGetConsumption: // syscall 41: get_cpu_time
            return sys_tcb_get_cpu_time_handler(args);
        case Syscall::BatonSwitch:
            return sys_baton_switch_handler(args);
        case Syscall::BatonInject:
            return sys_baton_inject_handler(args);
        case Syscall::Yield:
            return sys_yield_handler(args);
        case Syscall::TcbGetId:
            return sys_tcb_get_id_handler(args);
        case Syscall::TcbSetTimeoutNotification:
        case Syscall::BatonSetTimeoutNotification:
            return sys_baton_bind_notification_handler(args);
        case Syscall::TcbBindBaton:
            return sys_tcb_bind_baton_handler(args);
        case Syscall::NotificationDelegate:
            return sys_notification_bind_parent_handler(args);
        case Syscall::NotificationUndelegate:
            return sys_notification_unbind_parent_handler(args);
        case Syscall::BatonCreateIrq:
            return sys_baton_create_irq_handler(args);
        case Syscall::BatonSetBudget:
            return sys_baton_set_budget_handler(args);
        default:
            return SyscallResult(SYS_ERR_INVALID, 0);
    }
}

} // namespace oz
