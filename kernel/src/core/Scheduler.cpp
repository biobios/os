#include "core/Scheduler.hpp"
#include "core/Kernel.hpp"
#include "core/Syscall.hpp"

namespace oz {

Thread* Scheduler::initThread(Thread* thread, void* stack, std::uint64_t stack_size, void (*entry)(void*), void* arg) {
    thread->id = next_thread_id++;
    thread->stack_base = stack;
    thread->stack_size = stack_size;
    setupThreadContext(thread, entry, arg);
    return thread;
}

void Scheduler::setupThreadContext(Thread* thread, void (*entry)(void*), void* arg) {
    thread->gs_base = oz::x86_64::readMSR(0xC0000101);
    thread->kernel_gs_base = oz::x86_64::readMSR(0xC0000102);

    // Set up stack for switch_context
    std::uint64_t rsp = reinterpret_cast<std::uint64_t>(thread->stack_base) + thread->stack_size;
    rsp &= ~15ULL; // 16-byte align

    rsp -= sizeof(ThreadContext);
    thread->rsp = reinterpret_cast<void*>(rsp);

    ThreadContext* ctx = reinterpret_cast<ThreadContext*>(rsp);
    ctx->rip = reinterpret_cast<std::uint64_t>(&thread_stub);
    ctx->r12 = reinterpret_cast<std::uint64_t>(entry);
    ctx->rbx = reinterpret_cast<std::uint64_t>(arg);
    ctx->r13 = reinterpret_cast<std::uint64_t>(&Scheduler::exit_thread_stub);
    ctx->r14 = reinterpret_cast<std::uint64_t>(this); // Pass scheduler ptr
    ctx->r15 = 0;
    ctx->rbp = 0;
}

void Scheduler::initMainThread() {
    alignas(Thread) static std::uint8_t main_thread_storage[sizeof(Thread)];
    alignas(16) static std::uint8_t main_stack[4096];
    Thread* thread = new (main_thread_storage) Thread(
        next_thread_id++, main_stack, sizeof(main_stack), PageBlock<>{nullptr}, 0
    );
    thread->is_untyped_backed = true;
    thread->state = ThreadState::Blocked;
    thread->last_switched_in = oz::x86_64::rdtsc();
    current_thread = thread;
    main_thread = thread;
}

void Scheduler::perform_irq_timeout_unwind(Thread* handler) {
    Baton* irq_b = handler->borrowed_baton;
    if (irq_b && irq_b->is_irq_baton) {
        // 1. 借用バトンの強制解除
        irq_b->thread = nullptr;
        irq_b->state = BatonState::READY;
        irq_b->remaining = 0;
        handler->borrowed_baton = nullptr;
        handler->baton = handler->get_active_baton();
    }

    // 2. ネストスタックのポップ
    if (g_current_irq_thread == handler) {
        g_current_irq_thread = handler->next_nested_irq;
        handler->next_nested_irq = nullptr;
    }

    // 3. 親スケジューラ / 通知先へのタイムアウト非同期通知 (Notification)
    if (handler->bound_notification) {
        handler->bound_notification->signal(
            handler->sched_badge ? handler->sched_badge : (1ULL << 61));
    }

    // 4. ハンドラをタイムアウト中断状態 (Ready) に設定
    handler->is_blocked = false;
    handler->state = ThreadState::Ready;
    handler->sched_reason = SchedReason::TIMEOUT;

    // 5. 被割り込みスレッド T (または下位ハンドラ H1) の救出
    ExecutionContext* interrupted = handler->saved_frame.interrupted_context;
    std::uint32_t rem_ticks = handler->saved_frame.lapic_remaining_ticks;
    std::uint64_t entry_tsc = handler->saved_frame.entry_tsc;
    handler->saved_frame.clear();

    std::uint64_t now = oz::x86_64::rdtsc();
    // No Time Theft 補正:
    if (interrupted) {
        Thread* int_t = static_cast<Thread*>(interrupted);
        if (!int_t->is_irq_handling()) {
            interrupted->last_switched_in += (now - entry_tsc);
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

    // 6. 被害者 T へ救出復帰
    if (interrupted) {
        performIRQResume(interrupted, now);
    }
}

void Scheduler::tick() {
    InterruptGuard guard;
    ExecutionContext* prev = current_thread;
    if (!prev) return;
    std::uint64_t now = oz::x86_64::rdtsc();

    Thread* prev_t = static_cast<Thread*>(prev);
    if (prev_t->is_irq_handling()) {
        perform_irq_timeout_unwind(prev_t);
        return;
    }

    // デカルト木に基づくタイムアウト処理
    if (cartesian_root) {
        std::uint64_t slice = (prev->last_switched_in != 0) ? (now - prev->last_switched_in) : 0;
        std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
        std::uint64_t elapsed_us = slice / (tsc_per_us ? tsc_per_us : 1);

        Baton* expired = cartesian_root;

        if (expired == running_baton) {
            // 下位タイムアウト (末尾自体の枯渇)
            cartesianPopLeaf(cartesian_root, expired);
            expired->state = BatonState::READY;
            expired->remaining = 0;
            expired->is_active = false;
            expired->notify_if_runnable();

            prev->sched_reason = SchedReason::TIMEOUT;
            prev->state = ThreadState::Ready;

            Baton* caller_b = expired->caller_baton;
            ExecutionContext* caller_ctx = expired->caller_context;
            expired->caller_baton = nullptr;
            expired->caller_context = nullptr;
            if (caller_b) {
                caller_b->callee_baton = nullptr;
                caller_b->state = BatonState::RUNNING;
                cartesianPushDown(caller_b);
                running_baton = caller_b;
                if (!caller_ctx) caller_ctx = caller_b->thread;
            }
            if (caller_ctx) {
                caller_ctx->sched_reason = SchedReason::TIMEOUT;
                if (cartesian_root && !cartesian_root->is_infinite) {
                    oz::x86_64::setLAPICOneShot(cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
                } else {
                    oz::x86_64::stopLAPICTimer();
                }
                performContextSwitch(prev, caller_ctx, now);
            }
            return;
        } else {
            // 上位タイムアウト (親・祖先バトンの枯渇)
            expired->sub_min_baton = expired->cartesian_right;
            expired->tail_baton = running_baton;

            cartesianSplitAt(cartesian_root, expired, elapsed_us);
            expired->state = BatonState::WAIT_START;
            expired->remaining = 0;
            expired->is_active = false;
            expired->notify_if_runnable();

            prev->sched_reason = SchedReason::TIMEOUT;
            prev->state = ThreadState::Ready;

            Baton* caller_b = expired->caller_baton;
            ExecutionContext* caller_ctx = expired->caller_context;
            expired->caller_baton = nullptr;
            expired->caller_context = nullptr;
            if (caller_b) {
                caller_b->callee_baton = nullptr;
                caller_b->state = BatonState::RUNNING;
                cartesianPushDown(caller_b);
                running_baton = caller_b;
                if (!caller_ctx) caller_ctx = caller_b->thread;
            }
            if (caller_ctx) {
                caller_ctx->sched_reason = SchedReason::TIMEOUT;
                if (cartesian_root && !cartesian_root->is_infinite) {
                    oz::x86_64::setLAPICOneShot(cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
                } else {
                    oz::x86_64::stopLAPICTimer();
                }
                performContextSwitch(prev, caller_ctx, now);
            }
            return;
        }
    }

    // 通常スレッドの時間切れ: 呼出元 (Caller) が存在する場合、その Caller へ復帰 (Legacy fallback)
    if (prev->baton && prev->baton->caller_context) {
        std::uint64_t slice = (prev->last_switched_in != 0) ? (now - prev->last_switched_in) : 0;
        std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
        std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
        if (!prev->baton->is_infinite) {
            if (prev->baton->budget > consumed_us) {
                prev->baton->budget -= consumed_us;
                oz::x86_64::setLAPICOneShot(prev->baton->budget, oz::x86_64::APIC_TIMER_VECTOR);
                return;
            }
            prev->baton->budget = 0;
        }
        prev->sched_reason = SchedReason::TIMEOUT;
        prev->baton->is_active = false;
        prev->state = ThreadState::Ready;
        prev->baton->notify_if_runnable();

        ExecutionContext* caller = prev->baton->caller_context;
        prev->baton->caller_context = nullptr;
        caller->sched_reason = SchedReason::TIMEOUT;
        performContextSwitch(prev, caller, now);
        return;
    }
}

void Scheduler::performContextSwitch(ExecutionContext* prev_thread, ExecutionContext* next_thread, std::uint64_t now) {
    if (prev_thread) {
        if (prev_thread->state == ThreadState::Running) {
            prev_thread->state = ThreadState::Ready;
        }
        if (prev_thread->last_switched_in != 0) {
            std::uint64_t slice = now - prev_thread->last_switched_in;
            prev_thread->total_cpu_cycles += slice;
            prev_thread->last_consumed_cycles = slice;
        }
        prev_thread->last_switched_in = 0;
        prev_thread->gs_base = oz::x86_64::readMSR(0xC0000101);
        prev_thread->kernel_gs_base = oz::x86_64::readMSR(0xC0000102);
        prev_thread->syscall_user_rsp = g_syscall_context.user_rsp;
    }

    next_thread->state = ThreadState::Running;
    next_thread->last_switched_in = now;
    current_thread = next_thread;

    if (next_thread->gs_base != 0 || next_thread->kernel_gs_base != 0) {
        oz::x86_64::writeMSR(0xC0000101, next_thread->gs_base);
        oz::x86_64::writeMSR(0xC0000102, next_thread->kernel_gs_base);
    }
    
    std::uint64_t kernel_stack_top = reinterpret_cast<std::uint64_t>(next_thread->stack_base) + next_thread->stack_size;
    setSyscallKernelStack(kernel_stack_top);
    x86_64::setKernelStack(kernel_stack_top);
    g_syscall_context.user_rsp = next_thread->syscall_user_rsp;
    
    std::uint64_t target_cr3 = next_thread->pml4_phys.get();
    if (target_cr3 == 0 && getMasterPML4()) {
        target_cr3 = reinterpret_cast<std::uintptr_t>(getMasterPML4()) - oz::KERNEL_VIRT_OFFSET;
    }
    if (target_cr3 != 0) {
        std::uint64_t current_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
        if ((current_cr3 & ~0xFFFULL) != (target_cr3 & ~0xFFFULL)) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(target_cr3) : "memory");
        }
    }
    
    if (prev_thread) {
        if (prev_thread != next_thread) {
            switch_context(&prev_thread->rsp, next_thread->rsp);
        }
    } else {
        void* dummy_rsp;
        switch_context(&dummy_rsp, next_thread->rsp);
    }
}

[[noreturn]] void Scheduler::abandonCurrentContextTo(ExecutionContext* next_thread, std::uint64_t now) {
    if (!next_thread || next_thread->is_dead) {
        next_thread = main_thread;
    }

    next_thread->state = ThreadState::Running;
    next_thread->last_switched_in = now;
    current_thread = next_thread;

    if (next_thread->gs_base != 0 || next_thread->kernel_gs_base != 0) {
        oz::x86_64::writeMSR(0xC0000101, next_thread->gs_base);
        oz::x86_64::writeMSR(0xC0000102, next_thread->kernel_gs_base);
    }

    std::uint64_t kernel_stack_top = reinterpret_cast<std::uint64_t>(next_thread->stack_base) + next_thread->stack_size;
    setSyscallKernelStack(kernel_stack_top);
    x86_64::setKernelStack(kernel_stack_top);
    g_syscall_context.user_rsp = next_thread->syscall_user_rsp;

    std::uint64_t target_cr3 = next_thread->pml4_phys.get();
    if (target_cr3 == 0 && getMasterPML4()) {
        target_cr3 = reinterpret_cast<std::uintptr_t>(getMasterPML4()) - oz::KERNEL_VIRT_OFFSET;
    }
    if (target_cr3 != 0) {
        std::uint64_t current_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
        if ((current_cr3 & ~0xFFFULL) != (target_cr3 & ~0xFFFULL)) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(target_cr3) : "memory");
        }
    }

    if (cartesian_root && !cartesian_root->is_infinite) {
        oz::x86_64::setLAPICOneShot(cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
    } else {
        oz::x86_64::stopLAPICTimer();
    }

    load_context_noreturn(next_thread->rsp);
}

void Scheduler::performIRQSwitchTo(Thread* handler_thread, std::uint64_t now) {
    ExecutionContext* prev = current_thread;
    if (prev) {
        if (prev->state == ThreadState::Running) {
            prev->state = ThreadState::Ready;
        }
        prev->gs_base = oz::x86_64::readMSR(0xC0000101);
        prev->kernel_gs_base = oz::x86_64::readMSR(0xC0000102);
        prev->syscall_user_rsp = g_syscall_context.user_rsp;
    }

    handler_thread->state = ThreadState::Running;
    handler_thread->last_switched_in = now;
    current_thread = handler_thread;

    if (handler_thread->gs_base != 0 || handler_thread->kernel_gs_base != 0) {
        oz::x86_64::writeMSR(0xC0000101, handler_thread->gs_base);
        oz::x86_64::writeMSR(0xC0000102, handler_thread->kernel_gs_base);
    }

    std::uint64_t kernel_stack_top = reinterpret_cast<std::uint64_t>(handler_thread->stack_base) + handler_thread->stack_size;
    setSyscallKernelStack(kernel_stack_top);
    x86_64::setKernelStack(kernel_stack_top);
    g_syscall_context.user_rsp = handler_thread->syscall_user_rsp;

    if (handler_thread->pml4_phys.get() != 0) {
        std::uint64_t current_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
        if (current_cr3 != static_cast<std::uint64_t>(handler_thread->pml4_phys.get())) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(static_cast<std::uint64_t>(handler_thread->pml4_phys.get())));
        }
    }

    if (prev) {
        if (prev != handler_thread) {
            switch_context(&prev->rsp, handler_thread->rsp);
        }
    } else {
        void* dummy_rsp;
        switch_context(&dummy_rsp, handler_thread->rsp);
    }
}

void Scheduler::performIRQResume(ExecutionContext* interrupted, std::uint64_t now) {
    ExecutionContext* prev = current_thread;
    if (prev) {
        if (prev->state == ThreadState::Running) {
            prev->state = ThreadState::Ready;
        }
        if (prev->last_switched_in != 0) {
            std::uint64_t slice = now - prev->last_switched_in;
            prev->total_cpu_cycles += slice;
            prev->last_consumed_cycles = slice;
        }
        prev->last_switched_in = 0;
        prev->gs_base = oz::x86_64::readMSR(0xC0000101);
        prev->kernel_gs_base = oz::x86_64::readMSR(0xC0000102);
        prev->syscall_user_rsp = g_syscall_context.user_rsp;
    }

    interrupted->state = ThreadState::Running;
    current_thread = interrupted;

    if (interrupted->gs_base != 0 || interrupted->kernel_gs_base != 0) {
        oz::x86_64::writeMSR(0xC0000101, interrupted->gs_base);
        oz::x86_64::writeMSR(0xC0000102, interrupted->kernel_gs_base);
    }

    std::uint64_t kernel_stack_top = reinterpret_cast<std::uint64_t>(interrupted->stack_base) + interrupted->stack_size;
    setSyscallKernelStack(kernel_stack_top);
    x86_64::setKernelStack(kernel_stack_top);
    g_syscall_context.user_rsp = interrupted->syscall_user_rsp;

    if (interrupted->pml4_phys.get() != 0) {
        std::uint64_t current_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
        if (current_cr3 != static_cast<std::uint64_t>(interrupted->pml4_phys.get())) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(static_cast<std::uint64_t>(interrupted->pml4_phys.get())));
        }
    }

    if (prev) {
        if (prev != interrupted) {
            switch_context(&prev->rsp, interrupted->rsp);
        }
    } else {
        void* dummy_rsp;
        switch_context(&dummy_rsp, interrupted->rsp);
    }
}

void Scheduler::yieldToSchedulerBlocked(Thread* current, std::uint64_t now, Baton* target_baton) {
    if (!current) return;
    current->sched_reason = SchedReason::BLOCKED;
    Baton* b = target_baton ? target_baton : current->get_active_baton();
    if (b) {
        cartesianPopLeaf(cartesian_root, b);
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
            running_baton = caller_b;
            if (!caller) caller = caller_b->thread;
        }
        if (caller && caller != current) {
            caller->sched_reason = SchedReason::BLOCKED;
            if (cartesian_root && !cartesian_root->is_infinite) {
                oz::x86_64::setLAPICOneShot(cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
            } else {
                oz::x86_64::stopLAPICTimer();
            }
            performContextSwitch(current, caller, now);
            return;
        }
    }
    schedule();
}

void Scheduler::yieldToSchedulerOrphaned(Thread* current, Baton* b, std::uint64_t now) {
    if (!current) return;
    current->sched_reason = SchedReason::ORPHANED;
    if (b) {
        cartesianPopLeaf(cartesian_root, b);
        b->state = BatonState::READY;
        b->is_active = false;
        b->thread = nullptr;
        Baton* caller_b = b->caller_baton;
        ExecutionContext* caller = b->caller_context;
        b->caller_baton = nullptr;
        b->caller_context = nullptr;
        if (caller_b) {
            caller_b->callee_baton = nullptr;
            caller_b->state = BatonState::RUNNING;
            cartesianPushDown(caller_b);
            running_baton = caller_b;
            if (!caller) caller = caller_b->thread;
        }
        if (caller && caller != current) {
            caller->sched_reason = SchedReason::ORPHANED;
            if (cartesian_root && !cartesian_root->is_infinite) {
                oz::x86_64::setLAPICOneShot(cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
            } else {
                oz::x86_64::stopLAPICTimer();
            }
            performContextSwitch(current, caller, now);
            return;
        }
    }
    schedule();
}

void Scheduler::schedule() {
    InterruptGuard guard;
    std::uint64_t now = oz::x86_64::rdtsc();

    if (current_thread) {
        current_thread->sched_reason = SchedReason::BLOCKED;
        if (current_thread->baton) {
            Baton* b = current_thread->baton;
            cartesianPopLeaf(cartesian_root, b);
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
                running_baton = caller_b;
                if (!caller) caller = caller_b->thread;
            }
            if (caller && caller != current_thread) {
                caller->sched_reason = SchedReason::BLOCKED;
                if (cartesian_root && !cartesian_root->is_infinite) {
                    oz::x86_64::setLAPICOneShot(cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
                } else {
                    oz::x86_64::stopLAPICTimer();
                }
                performContextSwitch(current_thread, caller, now);
                return;
            }
        }
    }
}

void Scheduler::exitThread() {
    Thread* cur = getCurrentThread();
    if (cur && cur->is_irq_handling()) {
        perform_irq_timeout_unwind(cur);
        return;
    }
    if (cur && cur->bound_irq_handler) {
        if (cur->bound_irq_handler->handler_thread == cur) {
            cur->bound_irq_handler->handler_thread = nullptr;
            cur->bound_irq_handler->assigned_irq_baton = nullptr;
        }
        cur->bound_irq_handler = nullptr;
    }
    bool had_baton = false;
    ExecutionContext* caller = nullptr;
    {
        InterruptGuard guard;
        current_thread->state = ThreadState::Dead;
        current_thread->is_dead = true;
        
        if (current_thread->baton) {
            Baton* b = current_thread->baton;
            cartesianPopLeaf(cartesian_root, b);
            b->state = BatonState::READY;
            b->is_active = false;
            caller = b->caller_context;
            Baton* caller_b = b->caller_baton;
            b->caller_baton = nullptr;
            if (caller_b) {
                caller_b->callee_baton = nullptr;
                caller_b->state = BatonState::RUNNING;
                cartesianPushDown(caller_b);
                running_baton = caller_b;
                if (!caller) caller = caller_b->thread;
            }
            b->caller_context = nullptr;
            b->thread = nullptr;
            current_thread->baton = nullptr;
            had_baton = true;
        }

        // Add to zombie queue
        if (cur) {
            cur->next_zombie = nullptr;
            if (zombie_queue_tail) {
                zombie_queue_tail->next_zombie = cur;
                zombie_queue_tail = cur;
            } else {
                zombie_queue_head = zombie_queue_tail = cur;
            }
        }
    }

    reapZombies();
    
    if (caller) {
        InterruptGuard guard;
        std::uint64_t now = oz::x86_64::rdtsc();
        caller->sched_reason = had_baton ? SchedReason::ORPHANED : SchedReason::EXIT;
        if (cartesian_root && !cartesian_root->is_infinite) {
            oz::x86_64::setLAPICOneShot(cartesian_root->get_effective_remaining(), oz::x86_64::APIC_TIMER_VECTOR);
        } else {
            oz::x86_64::stopLAPICTimer();
        }
        performContextSwitch(current_thread, caller, now);
        while (1) __asm__ volatile("cli; hlt");
    }

    while (1) __asm__ volatile("cli; hlt");
}

void Scheduler::reapZombies() {
    Thread* current_zombie;
    {
        InterruptGuard guard;
        current_zombie = zombie_queue_head;
        zombie_queue_head = nullptr;
        zombie_queue_tail = nullptr;
    }
    
    while (current_zombie) {
        Thread* next_z = current_zombie->next_zombie;
        
        if (current_zombie->cleanup) {
            current_zombie->cleanup(current_zombie->user_data);
        }
        
        current_zombie = next_z;
    }
}

void Scheduler::directSwitchTo(ExecutionContext* next_thread) {
    if (!next_thread || next_thread == current_thread) return;
    InterruptGuard guard;
    ExecutionContext* prev_thread = current_thread;
    std::uint64_t now = oz::x86_64::rdtsc();
    performContextSwitch(prev_thread, next_thread, now);
}

} // namespace oz
