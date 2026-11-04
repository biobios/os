#include "core/syscall/SyscallHandlers.hpp"

namespace oz {

static SyscallResult sys_debug_print_handler(const SyscallArgs& args) {
    // debug_print: write string to serial (secured with pointer validation)
    if (!is_user_address(args.rdi)) return -1;
    const char* str = reinterpret_cast<const char*>(args.rdi);
    constexpr std::size_t MAX_DEBUG_LEN = 4096;
    for (std::size_t i = 0; i < MAX_DEBUG_LEN; ++i) {
        if (!is_user_address(args.rdi + i)) break;
        char c = str[i];
        if (c == '\0') break;
        char buf[2] = {c, '\0'};
        dprint(buf);
    }
    return 0;
}

static SyscallResult sys_irq_handler_create_handler(const SyscallArgs& args) {
    // irq_handler_create(untyped_cap, vector, notification_ep_cap, target_cspace, target_slot)
    CSpace* current_cs = get_cspace();
    if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

    std::uint8_t vector = static_cast<std::uint8_t>(args.rsi);
    // Whitelist check: only allow external IRQs defined in IDT (0x30 - 0x4F)
    if (vector < 0x30 || vector > 0x4F) {
        return SyscallResult(SYS_ERR_INVALID, 0);
    }
    // Conflict check: vector must not be already in use
    if (oz::irq::getHandler(vector) != nullptr && oz::irq::getHandler(vector)->isRegistered()) {
        return SyscallResult(SYS_ERR_BUSY, 0);
    }

    CapID target_cs_id = static_cast<CapID>(args.r10);
    CapID target_slot = static_cast<CapID>(args.r8);
    if (target_slot == 0 || target_slot >= CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
    if (is_reserved_system_slot(target_slot)) return SyscallResult(SYS_ERR_INVALID, 0);

    CSpace* target_cs = resolve_target_cspace(current_cs, target_cs_id);
    if (!target_cs) return SyscallResult(SYS_ERR_PERM, 0);

    Capability* target_slot_cap = target_cs->getSlot(target_slot);
    if (!target_slot_cap || !target_slot_cap->is_null()) {
        return SyscallResult(SYS_ERR_BUSY, 0);
    }

    Capability* ut_slot = nullptr;
    if (args.rdi != 0 && args.rdi != INVALID_CAP_ID) {
        ut_slot = current_cs->getSlot(static_cast<CapID>(args.rdi));
        if (!ut_slot || ut_slot->type() != CapType::UntypedMemory || (!ut_slot->has_rights(CapRights::WRITE) && !ut_slot->has_rights(CapRights::DERIVE))) {
            return SyscallResult(SYS_ERR_PERM, 0);
        }
    }

    IRQHandler& handler = oz::g_irq_handlers[vector];
    handler.vector = vector;
    handler.unregisterAndReset();

    if (args.rdx != 0 && args.rdx != INVALID_CAP_ID) {
        Capability* ep_or_notif_slot = current_cs->getSlot(static_cast<CapID>(args.rdx));
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
}

static SyscallResult sys_irq_ack_legacy_handler(const SyscallArgs& args) {
    // irq_ack legacy
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    Capability* slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!slot || slot->type() != CapType::IRQHandler || !slot->has_rights(CapRights::ACK_WAIT)) {
        return -1;
    }
    std::uint8_t vector = slot->irq_vector();
    IRQHandler& handler = oz::g_irq_handlers[vector];
    if (!handler.acknowledge()) return -1;
    return 0;
}

static SyscallResult sys_ioport_create_handler(const SyscallArgs& args) {
    // ioport_create(parent_ioport_cap, base_port, port_count, target_cspace, target_slot)
    CSpace* current_cs = get_cspace();
    if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

    Capability* parent_slot = current_cs->getSlot(static_cast<CapID>(args.rdi));
    if (!parent_slot || parent_slot->type() != CapType::IOPort || !parent_slot->has_rights(CapRights::DERIVE)) {
        return SyscallResult(SYS_ERR_PERM, 0);
    }

    std::uint16_t parent_base = parent_slot->ioport_base();
    std::uint32_t parent_count = parent_slot->ioport_count();

    std::uint16_t base_port = static_cast<std::uint16_t>(args.rsi);
    std::uint32_t port_count = static_cast<std::uint32_t>(args.rdx);

    if (port_count == 0 || base_port < parent_base ||
        static_cast<std::uint32_t>(base_port) + port_count > static_cast<std::uint32_t>(parent_base) + parent_count) {
        return SyscallResult(SYS_ERR_INVALID, 0);
    }

    CapID target_cs_id = static_cast<CapID>(args.r10);
    CapID target_slot = static_cast<CapID>(args.r8);
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
}

static SyscallResult sys_ioport_read_handler(const SyscallArgs& args) {
    // ioport_read(ioport_cap, port, size)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    Capability* slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!slot || slot->type() != CapType::IOPort || !slot->has_rights(CapRights::IN)) {
        return -1;
    }

    std::uint16_t base_port = slot->ioport_base();
    std::uint32_t port_count = slot->ioport_count();
    std::uint16_t port = static_cast<std::uint16_t>(args.rsi);
    std::uint64_t size = args.rdx;

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
}

static SyscallResult sys_ioport_write_handler(const SyscallArgs& args) {
    // ioport_write(ioport_cap, port, value, size)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    Capability* slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!slot || slot->type() != CapType::IOPort || !slot->has_rights(CapRights::OUT)) {
        return -1;
    }

    std::uint16_t base_port = slot->ioport_base();
    std::uint32_t port_count = slot->ioport_count();
    std::uint16_t port = static_cast<std::uint16_t>(args.rsi);
    std::uint64_t size = args.r10;

    if (port < base_port || static_cast<std::uint32_t>(port) + size > static_cast<std::uint32_t>(base_port) + port_count) {
        return -1;
    }

    if (size == 1) {
        oz::x86_64::writeIO8(port, static_cast<std::uint8_t>(args.rdx));
    } else if (size == 2) {
        oz::x86_64::writeIO16(port, static_cast<std::uint16_t>(args.rdx));
    } else if (size == 4) {
        oz::x86_64::writeIO32(port, static_cast<std::uint32_t>(args.rdx));
    } else {
        return -1;
    }
    return 0;
}

static SyscallResult sys_irq_ack_handler(const SyscallArgs& args) {
    // sys_irq_ack(irq_handler_cap)
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(-1, 0);

    Capability* slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!slot || slot->type() != CapType::IRQHandler || !slot->has_rights(CapRights::ACK_WAIT)) {
        return SyscallResult(-1, 0);
    }
    std::uint8_t vector = slot->irq_vector();
    IRQHandler& handler = oz::g_irq_handlers[vector];
    handler.acknowledge();
    return SyscallResult(0, 0);
}

static SyscallResult sys_irq_bind_notification_handler(const SyscallArgs& args) {
    // sys_irq_handler_set_notification(irq_handler_cap, notif_cap, badge)
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(-1, 0);

    Capability* slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!slot || slot->type() != CapType::IRQHandler || !slot->has_rights(CapRights::BIND)) {
        return SyscallResult(-1, 0);
    }
    std::uint8_t vector = slot->irq_vector();
    IRQHandler* handler = &oz::g_irq_handlers[vector];

    Notification* notif = nullptr;
    if (static_cast<CapID>(args.rsi) != INVALID_CAP_ID && args.rsi != 0) {
        notif = get_cap_typed<Notification>(
            cspace, static_cast<CapID>(args.rsi), ObjectType::Notification, CapRights::WRITE);
        if (!notif) return SyscallResult(-1, 0);
    }

    handler->setNotification(notif, args.rdx);
    return SyscallResult(0, 0);
}

static SyscallResult sys_irq_wait_handler(const SyscallArgs& args) {
    // sys_irq_wait(irq_handler_cap, irq_baton_cap)
    // rdi: irq_handler_cap, rsi: irq_baton_cap
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    Thread* current = get_current_thread();
    if (!current) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    Capability* slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!slot || slot->type() != CapType::IRQHandler || !slot->has_rights(CapRights::ACK_WAIT)) {
        return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
    }
    std::uint8_t vector = slot->irq_vector();
    IRQHandler* handler = &oz::g_irq_handlers[vector];

    Baton* next_irq_baton = get_cap_typed<Baton>(
        cspace, static_cast<CapID>(args.rsi), ObjectType::Baton, CapRights::USE);

    if (!handler || !next_irq_baton) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    // 1. 引数の irq_baton の妥当性検査 (不変条件 1 & 2)
    if (!next_irq_baton->is_irq_baton) {
        return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_TYPE), 0);
    }
    if (next_irq_baton->thread && next_irq_baton->thread != current) {
        return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_BUSY), 0);
    }
    if (handler->handler_thread && handler->handler_thread != current) {
        return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_BUSY), 0);
    }

    bool was_handling_irq = false;
    Baton* prev_borrowed = current->borrowed_baton;

    // 2. 現在の借用状態の検査と消費クォンタムの減算
    if (prev_borrowed) {
        if (!prev_borrowed->is_irq_baton) {
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID_STATE), 0);
        }

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
            Kernel::instance().getScheduler().perform_irq_timeout_unwind(current);
            return SyscallResult(0, 0);
        } else {
            handler->mask();

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

            handler->handler_thread = nullptr;
            current->bound_irq_handler = nullptr;

            return SyscallResult(0, 0);
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
    handler->acknowledge();

    std::uint64_t now = oz::x86_64::rdtsc();

    if (!was_handling_irq) {
        Kernel::instance().getScheduler().yieldToSchedulerBlocked(current, now);
    } else {
        ExecutionContext* interrupted = current->saved_frame.interrupted_context;
        std::uint32_t rem_ticks = current->saved_frame.lapic_remaining_ticks;
        std::uint64_t entry_tsc = current->saved_frame.entry_tsc;
        current->saved_frame.clear();

        if (g_current_irq_thread == current) {
            g_current_irq_thread = current->next_nested_irq;
            current->next_nested_irq = nullptr;
        }

        if (interrupted) {
            Thread* int_t = static_cast<Thread*>(interrupted);
            if (!int_t->is_irq_handling()) {
                std::uint64_t irq_duration = now - entry_tsc;
                interrupted->last_switched_in += irq_duration;
            }
        }

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
}

SyscallResult handleSyscallHardware(const SyscallArgs& args) {
    switch (args.number) {
        case Syscall::Write:
            return sys_debug_print_handler(args);
        case Syscall::IrqHandlerCreate:
            return sys_irq_handler_create_handler(args);
        case Syscall::IrqAckLegacy:
            return sys_irq_ack_legacy_handler(args);
        case Syscall::IOPortCreate:
            return sys_ioport_create_handler(args);
        case Syscall::IOPortIn:
            return sys_ioport_read_handler(args);
        case Syscall::IOPortOut:
            return sys_ioport_write_handler(args);
        case Syscall::IrqAck:
            return sys_irq_ack_handler(args);
        case Syscall::IrqBindNotification:
            return sys_irq_bind_notification_handler(args);
        case Syscall::IrqWait:
            return sys_irq_wait_handler(args);
        default:
            return SyscallResult(SYS_ERR_INVALID, 0);
    }
}

} // namespace oz
