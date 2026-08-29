#pragma once
#include <cstdint>

namespace oz {

// Thread context saved on the stack during a context switch.
// Ordered from lowest address (top of stack) to highest address.
struct ThreadContext {
    std::uint64_t r15;
    std::uint64_t r14;
    std::uint64_t r13;
    std::uint64_t r12;
    std::uint64_t rbp;
    std::uint64_t rbx;
    std::uint64_t rip; // Pushed by call instruction
} __attribute__((packed));

enum class ThreadState {
    Ready,
    Running,
    Blocked,
    Dead
};

class Thread {
public:
    std::uint64_t id;
    void* rsp;
    void* stack_base;
    std::uint64_t stack_size;
    ThreadState state;
    Thread* next;        // For Scheduler ready queue
    Thread* next_waiter; // For Mutex wait queue

    Thread(std::uint64_t id, void* stack, std::uint64_t size)
        : id(id), rsp(reinterpret_cast<std::uint8_t*>(stack) + size), 
          stack_base(stack), stack_size(size), state(ThreadState::Ready), 
          next(nullptr), next_waiter(nullptr) {}
};

extern "C" void switch_context(void** old_rsp, void* new_rsp);
extern "C" void thread_stub();

} // namespace oz
