#include "core/Scheduler.hpp"
#include <new>

namespace oz {

// Global scheduler instance
static Scheduler g_scheduler;

Scheduler& getScheduler() {
    return g_scheduler;
}

Scheduler::Scheduler()
    : ready_queue_head(nullptr), ready_queue_tail(nullptr), current_thread(nullptr), next_tid(1),
      kernel_thread(0, nullptr, 0) {
    kernel_thread.state = ThreadState::Running;
    current_thread = &kernel_thread;
}

extern "C" void thread_stub();

Thread* Scheduler::createThread(void* stack, std::uint64_t stack_size, void (*entry)(void*), void* arg) {
    // Place the Thread object at the bottom of the stack memory to avoid separate allocation
    std::uint64_t thread_obj_addr = reinterpret_cast<std::uint64_t>(stack);
    thread_obj_addr = (thread_obj_addr + 7) & ~7ULL; // 8-byte align
    
    Thread* thread = new (reinterpret_cast<void*>(thread_obj_addr)) Thread(next_tid++, stack, stack_size);

    // Setup initial stack for switch_context
    std::uint8_t* stack_top = reinterpret_cast<std::uint8_t*>(stack) + stack_size;
    std::uint64_t rsp = reinterpret_cast<std::uint64_t>(stack_top) & ~15ULL; // 16-byte align
    
    // Push a dummy return address just in case
    // [REMOVED for System V ABI 16-byte alignment before call]
    // rsp -= 8;
    // *reinterpret_cast<std::uint64_t*>(rsp) = 0;

    // Allocate space for ThreadContext
    rsp -= sizeof(ThreadContext);
    ThreadContext* ctx = reinterpret_cast<ThreadContext*>(rsp);
    
    // Configure context to jump to thread_stub
    ctx->rip = reinterpret_cast<std::uint64_t>(thread_stub);
    
    // We use RBX for the argument, and R12 for the entry point
    ctx->rbx = reinterpret_cast<std::uint64_t>(arg);
    ctx->r12 = reinterpret_cast<std::uint64_t>(entry);
    
    // Initialize other callee-saved registers to 0
    ctx->rbp = 0;
    ctx->r13 = 0;
    ctx->r14 = 0;
    ctx->r15 = 0;
    
    thread->rsp = reinterpret_cast<void*>(rsp);
    return thread;
}

void Scheduler::queueThread(Thread* thread) {
    std::uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");

    thread->state = ThreadState::Ready;
    thread->next = nullptr;
    
    if (ready_queue_tail) {
        ready_queue_tail->next = thread;
        ready_queue_tail = thread;
    } else {
        ready_queue_head = ready_queue_tail = thread;
    }
    
    __asm__ volatile("pushq %0; popfq" :: "r"(rflags) : "memory", "cc");
}

void Scheduler::schedule() {
    std::uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");

    if (!ready_queue_head) {
        __asm__ volatile("pushq %0; popfq" :: "r"(rflags) : "memory", "cc");
        return; // No other threads to run
    }

    Thread* prev_thread = current_thread;
    Thread* next_thread = ready_queue_head;
    
    // Pop the next thread from the ready queue
    ready_queue_head = next_thread->next;
    if (!ready_queue_head) {
        ready_queue_tail = nullptr;
    }
    
    // Re-queue the current thread if it's still running
    if (prev_thread->state == ThreadState::Running) {
        queueThread(prev_thread);
    }
    
    next_thread->state = ThreadState::Running;
    current_thread = next_thread;
    
    // Perform context switch
    switch_context(&prev_thread->rsp, next_thread->rsp);
    
    // When execution returns here, we are in the context of the newly scheduled thread!
    __asm__ volatile("pushq %0; popfq" :: "r"(rflags) : "memory", "cc");
}

void Scheduler::exitThread() {
    std::uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");

    current_thread->state = ThreadState::Dead;
    
    __asm__ volatile("pushq %0; popfq" :: "r"(rflags) : "memory", "cc");
    
    // schedule() will not re-queue a Dead thread
    schedule();
    
    // Should never reach here
    while (1) __asm__ volatile("cli; hlt");
}

void Scheduler::start() {
    // start() can be used to set up the timer interrupt and start scheduling.
}

} // namespace oz

extern "C" void oz_exit_current_thread() {
    oz::getScheduler().exitThread();
}

#include "hardware/x86_64.hpp"

// Global timer interrupt handler for preemption
__attribute__((interrupt))
void timerInterruptHandler(void*) {
    oz::x86_64::notifyEndOfInterrupt();
    oz::getScheduler().schedule();
}

namespace oz {
    void initSchedulerTimer() {
        // Register the interrupt handler at vector 32
        oz::x86_64::setInterruptDescriptor(32, reinterpret_cast<void*>(timerInterruptHandler));
        // Initialize the APIC Timer to fire periodically
        oz::x86_64::initAPICTimer(32);
    }
}
