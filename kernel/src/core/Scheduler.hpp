#pragma once
#include "core/Thread.hpp"
#include "core/IScheduler.hpp"
#include "core/Syscall.hpp"
#include "hardware/x86_64.hpp"
#include "utils/InterruptGuard.hpp"
#include <cstdint>
#include <new>

namespace oz {

class Scheduler {
private:
    Thread* ready_queue_head = nullptr;
    Thread* ready_queue_tail = nullptr;
    Thread* current_thread = nullptr;
    
    Thread* zombie_queue_head = nullptr;
    Thread* zombie_queue_tail = nullptr;
    
    Thread* sleep_queue_head = nullptr;
    std::uint64_t tick_count = 0;

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

    void initMainThread() {
        alignas(Thread) static std::uint8_t main_thread_storage[sizeof(Thread)];
        Thread* thread = new (main_thread_storage) Thread(
            next_thread_id++, nullptr, 0, PageBlock<>{nullptr}, 0
        );
        thread->is_untyped_backed = true;
        thread->state = ThreadState::Running;
        current_thread = thread;
    }

    void queueThread(Thread* thread) {
        InterruptGuard guard;

        thread->state = ThreadState::Ready;
        thread->next = nullptr;
        
        if (ready_queue_tail) {
            ready_queue_tail->next = thread;
            ready_queue_tail = thread;
        } else {
            ready_queue_head = ready_queue_tail = thread;
        }
    }

    std::uint64_t getTicks() const {
        return tick_count;
    }

    void tick() {
        {
            InterruptGuard guard;
            tick_count++;
            
            Thread* current_sleep = sleep_queue_head;
            Thread* prev_sleep = nullptr;
            
            while (current_sleep) {
                if (tick_count >= current_sleep->wake_up_tick) {
                    Thread* to_wake = current_sleep;
                    current_sleep = current_sleep->next_sleep;
                    
                    if (prev_sleep) {
                        prev_sleep->next_sleep = current_sleep;
                    } else {
                        sleep_queue_head = current_sleep;
                    }
                    
                    // Add back to ready queue
                    to_wake->state = ThreadState::Ready;
                    to_wake->next = nullptr;
                    if (ready_queue_tail) {
                        ready_queue_tail->next = to_wake;
                        ready_queue_tail = to_wake;
                    } else {
                        ready_queue_head = ready_queue_tail = to_wake;
                    }
                } else {
                    prev_sleep = current_sleep;
                    current_sleep = current_sleep->next_sleep;
                }
            }
        } // guard destroyed
        
        schedule();
    }

    void sleep(std::uint64_t ticks) {
        {
            InterruptGuard guard;
            current_thread->state = ThreadState::Blocked;
            current_thread->wake_up_tick = tick_count + ticks;
            current_thread->next_sleep = sleep_queue_head;
            sleep_queue_head = current_thread;
        }
        schedule();
    }

    void schedule() {
        InterruptGuard guard;

        if (!ready_queue_head) {
            return;
        }

        Thread* prev_thread = current_thread;
        Thread* next_thread = ready_queue_head;
        
        ready_queue_head = next_thread->next;
        if (!ready_queue_head) {
            ready_queue_tail = nullptr;
        }
        
        if (prev_thread && prev_thread->state == ThreadState::Running) {
            // Need to temporarily unlock to queue? No, queueThread uses its own guard, but wait...
            // If queueThread has a guard, and schedule has a guard, it will push/pop rflags again. 
            // pushfq/popq is safe to nest!
            queueThread(prev_thread);
        }
        
        if (prev_thread) {
            prev_thread->gs_base = oz::x86_64::readMSR(0xC0000101);
            prev_thread->kernel_gs_base = oz::x86_64::readMSR(0xC0000102);
            // Save the user RSP from the global SyscallContext
            prev_thread->syscall_user_rsp = g_syscall_context.user_rsp;
        }

        next_thread->state = ThreadState::Running;
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
            switch_context(&prev_thread->rsp, next_thread->rsp);
        } else {
            void* dummy_rsp;
            switch_context(&dummy_rsp, next_thread->rsp);
        }
    }

    void exitThread() {
        {
            InterruptGuard guard;
            current_thread->state = ThreadState::Dead;
            
            // Add to zombie queue
            current_thread->next_zombie = nullptr;
            if (zombie_queue_tail) {
                zombie_queue_tail->next_zombie = current_thread;
                zombie_queue_tail = current_thread;
            } else {
                zombie_queue_head = zombie_queue_tail = current_thread;
            }
        }
        
        schedule();
        
        while (1) __asm__ volatile("cli; hlt");
    }
    
    // Reaper / Idle thread entry
    static void idle_reaper_task(void* arg) {
        Scheduler* self = static_cast<Scheduler*>(arg);
        while (1) {
            self->reapZombies();
            __asm__ volatile("sti; hlt");
        }
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

    Thread* getCurrentThread() { return current_thread; }
};

static_assert(scheduler<Scheduler>);

} // namespace oz
