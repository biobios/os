#pragma once
#include "core/Thread.hpp"
#include "core/Baton.hpp"
#include "core/IScheduler.hpp"
#include "core/IRQHandler.hpp"
#include "core/Notification.hpp"
#include "core/SavedInterruptFrame.hpp"
#include "core/CartesianTree.hpp"
#include "hardware/x86_64.hpp"
#include "utils/InterruptGuard.hpp"
#include <cstdint>
#include <new>

void dprint(const char*);
void dprint_hex(std::uint64_t);
void dprint_dec(std::uint64_t);

namespace oz {

PageTable* getMasterPML4();

class Scheduler {
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

    Baton* cartesian_root{nullptr};
    Baton* running_baton{nullptr};
    Thread* main_thread{nullptr};

    Thread* initThread(Thread* thread, void* stack, std::uint64_t stack_size, void (*entry)(void*), void* arg);
    void setupThreadContext(Thread* thread, void (*entry)(void*), void* arg);
    void initMainThread();

    void perform_irq_timeout_unwind(Thread* handler);
    void tick();

    void performContextSwitch(ExecutionContext* prev_thread, ExecutionContext* next_thread, std::uint64_t now);
    [[noreturn]] void abandonCurrentContextTo(ExecutionContext* next_thread, std::uint64_t now);
    void performIRQSwitchTo(Thread* handler_thread, std::uint64_t now);
    void performIRQResume(ExecutionContext* interrupted, std::uint64_t now);

    void yieldToSchedulerBlocked(Thread* current, std::uint64_t now, Baton* target_baton = nullptr);
    void yieldToSchedulerOrphaned(Thread* current, Baton* b, std::uint64_t now);

    void schedule();
    void exitThread();
    void reapZombies();
    void directSwitchTo(ExecutionContext* next_thread);

    ExecutionContext* getCurrentContext() { return current_thread; }
    Thread* getCurrentThread() {
        return current_thread ? static_cast<Thread*>(current_thread) : nullptr;
    }
};

static_assert(scheduler<Scheduler>);

extern Scheduler* g_scheduler_ptr;

} // namespace oz
