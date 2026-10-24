#pragma once
#include "core/Thread.hpp"
#include "core/InterruptThread.hpp"
#include "core/Baton.hpp"
#include "core/IScheduler.hpp"
#include "core/Syscall.hpp"
#include "core/Notification.hpp"
#include "core/SavedInterruptFrame.hpp"
#include "hardware/x86_64.hpp"
#include "utils/InterruptGuard.hpp"
#include <cstdint>
#include <new>

void dprint(const char*);
void dprint_hex(std::uint64_t);
void dprint_dec(std::uint64_t);

namespace oz {

class Scheduler {
public:

private:
    ExecutionContext* current_thread = nullptr;
    
    Thread* zombie_queue_head = nullptr;
    Thread* zombie_queue_tail = nullptr;
    
    std::uint64_t next_thread_id = 1;

    static void exit_thread_stub(Scheduler* self) {
        self->exitThread();
    }

public:
    Scheduler() = default;

    Thread* initThread(Thread* thread, void* stack, std::uint64_t stack_size, void (*entry)(void*), void* arg) {
        thread->id = next_thread_id++;
        thread->stack_base = stack;
        thread->stack_size = stack_size;
        setupThreadContext(thread, entry, arg);
        return thread;
    }

    void setupThreadContext(Thread* thread, void (*entry)(void*), void* arg) {
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

    Thread* main_thread{nullptr};

    void initMainThread() {
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

    void perform_clean_unwind(InterruptThread* handler) {
        // 1. 退避スタックから直前の中断コンテキストを取り出す
        ExecutionContext* interrupted = handler->saved_frame.interrupted_context;
        std::uint32_t rem_ticks = handler->saved_frame.lapic_remaining_ticks;
        handler->saved_frame.clear();

        if (g_current_interrupt_thread == handler) {
            g_current_interrupt_thread = handler->next_nested;
            handler->next_nested = nullptr;
        }

        // 2. タイマ停止
        oz::x86_64::stopLAPICTimer();

        // 3. タイムアウトしたハンドラを停止状態（中断状態）にする
        if (handler->baton) {
            handler->baton->budget = 0;
            handler->baton->is_active = false;
        }
        handler->is_waiting_irq = false;
        handler->is_interrupted = true;
        handler->is_blocked = true;
        handler->state = ThreadState::Blocked;

        std::uint64_t now = oz::x86_64::rdtsc();

        // 4. コンテキストの復帰と通知
        if (interrupted) {
            // [ケース1: 割り込み直後の枯渇]
            if (handler->baton && handler->baton->bound_notif) {
                handler->baton->bound_notif->signal(handler->baton->bound_badge);
            }

            // 被害者 T（またはネストされた下位ハンドラ）を救出復帰
            if (rem_ticks > 0) {
                oz::x86_64::setLAPICRemainingTicks(rem_ticks, oz::x86_64::APIC_TIMER_VECTOR);
            }

            interrupted->last_switched_in = now;
            performContextSwitch(handler, interrupted, now);
        } else {
            // [ケース2: スケジューラ再開後の 2 回目枯渇]
            ExecutionContext* caller = handler->caller_context;
            handler->caller_context = nullptr;
            if (caller) {
                caller->setSyscallReturnValue(SYS_BATON_SWITCH_TIMEOUT);
                caller->last_switched_in = now;
                performContextSwitch(handler, caller, now);
            }
        }
    }

    void tick() {
        InterruptGuard guard;
        ExecutionContext* prev = current_thread;
        if (!prev) return;
        std::uint64_t now = oz::x86_64::rdtsc();

        // 割り込みハンドラの時間切れ（Clean Unwind 方式）
        if (prev->isInterruptThread()) {
            perform_clean_unwind(static_cast<InterruptThread*>(prev));
            return;
        }

        // 通常スレッドの時間切れ: 呼出元 (Caller) が存在する場合、その Caller へ復帰
        if (prev->baton && prev->baton->caller_context) {
            std::uint64_t slice = (prev->last_switched_in != 0) ? (now - prev->last_switched_in) : 0;
            std::uint64_t tsc_per_us = oz::x86_64::getTSCTicksPerUs();
            std::uint64_t consumed_us = slice / (tsc_per_us ? tsc_per_us : 1);
            if (!prev->baton->is_infinite) {
                if (prev->baton->budget > consumed_us) {
                    prev->baton->budget -= consumed_us;
                    // まだ時間予算が残っている場合はタイマーを再設定して継続走行
                    oz::x86_64::setLAPICOneShot(prev->baton->budget, oz::x86_64::APIC_TIMER_VECTOR);
                    return;
                }
                prev->baton->budget = 0;
            }
            prev->last_consumed_cycles = slice;
            prev->total_cpu_cycles += slice;
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

public:
    void performContextSwitch(ExecutionContext* prev_thread, ExecutionContext* next_thread, std::uint64_t now) {
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
            // Save the user RSP from the global SyscallContext
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
        // Restore the user RSP for the next thread
        g_syscall_context.user_rsp = next_thread->syscall_user_rsp;
        
        if (next_thread->pml4_phys.get() != 0) {
            std::uint64_t current_cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
            if (current_cr3 != static_cast<std::uint64_t>(next_thread->pml4_phys.get())) {
                __asm__ volatile("mov %0, %%cr3" :: "r"(static_cast<std::uint64_t>(next_thread->pml4_phys.get())));
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

    void schedule() {
        InterruptGuard guard;
        std::uint64_t now = oz::x86_64::rdtsc();

        if (current_thread) {
            current_thread->sched_reason = SchedReason::BLOCKED;
            if (current_thread->baton) {
                current_thread->baton->is_active = false;
                if (current_thread->baton->caller_context) {
                    ExecutionContext* caller = current_thread->baton->caller_context;
                    current_thread->baton->caller_context = nullptr;
                    caller->sched_reason = SchedReason::BLOCKED;
                    performContextSwitch(current_thread, caller, now);
                    return;
                }
            }
        }
    }

    void exitThread() {
        bool had_baton = false;
        Thread* cur = getCurrentThread();
        ExecutionContext* caller = nullptr;
        {
            InterruptGuard guard;
            current_thread->state = ThreadState::Dead;
            current_thread->is_dead = true;
            
            if (current_thread->baton) {
                caller = current_thread->baton->caller_context;
                current_thread->baton->caller_context = nullptr;
                current_thread->baton->thread = nullptr;
                current_thread->baton->is_active = false;
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
            performContextSwitch(current_thread, caller, now);
            while (1) __asm__ volatile("cli; hlt");
        }

        while (1) __asm__ volatile("cli; hlt");
    }

    void reapZombies() {
        Thread* current_zombie;
        {
            InterruptGuard guard;
            current_zombie = zombie_queue_head;
            zombie_queue_head = nullptr;
            zombie_queue_tail = nullptr;
        }
        
        while (current_zombie) {
            Thread* next_z = current_zombie->next_zombie;
            
            // Call cleanup callback if any
            if (current_zombie->cleanup) {
                current_zombie->cleanup(current_zombie->user_data);
            }
            
            current_zombie = next_z;
        }
    }

    void directSwitchTo(ExecutionContext* next_thread) {
        if (!next_thread || next_thread == current_thread) return;
        InterruptGuard guard;
        ExecutionContext* prev_thread = current_thread;
        std::uint64_t now = oz::x86_64::rdtsc();
        performContextSwitch(prev_thread, next_thread, now);
    }

    ExecutionContext* getCurrentContext() { return current_thread; }
    Thread* getCurrentThread() {
        return (current_thread && !current_thread->isInterruptThread()) ? static_cast<Thread*>(current_thread) : nullptr;
    }
};

static_assert(scheduler<Scheduler>);

extern Scheduler* g_scheduler_ptr;

} // namespace oz
