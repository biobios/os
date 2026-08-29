#pragma once
#include "core/Thread.hpp"

namespace oz {

class Scheduler {
private:
    Thread* ready_queue_head;
    Thread* ready_queue_tail;
    Thread* current_thread;
    std::uint64_t next_tid;
    
    // A dummy thread to hold the initial kernel execution context
    Thread kernel_thread;

public:
    Scheduler();

    // Create and initialize a Thread object.
    // The Thread object itself is placed at the bottom (lowest address) of the provided stack memory.
    Thread* createThread(void* stack, std::uint64_t stack_size, void (*entry)(void*), void* arg);

    // Add a thread to the ready queue
    void queueThread(Thread* thread);

    // Select next thread and switch to it
    void schedule();

    // Terminate the current thread
    void exitThread();

    Thread* getCurrentThread() { return current_thread; }

    // Start the scheduler (called once during kernel initialization)
    void start();
};

// Get the global scheduler instance
Scheduler& getScheduler();

// Initialize and start the APIC timer for preemptive scheduling
void initSchedulerTimer();

} // namespace oz
