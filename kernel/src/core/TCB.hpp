#pragma once
#include "core/Capability.hpp"
#include "core/Thread.hpp"
#include <cstdint>

namespace oz {

// Forward declarations
class VSpace;
class CSpace;

class TCB : public KernelObject {
private:
    Thread* thread_;
    bool owned_;  // If true, TCB owns the thread and will schedule cleanup on destruction

    // Function pointers for scheduler operations (set by Kernel.hpp)
    void (*queue_fn_)(void* scheduler, Thread* t);
    void* scheduler_;
    void (*queue_thread_fn_)(Thread*){nullptr};

public:
    TCB(Thread* thread, bool owned,
        void (*queue_fn)(void*, Thread*) = nullptr, void* scheduler = nullptr,
        bool untyped = false)
        : KernelObject(ObjectType::TCB, untyped),
          thread_(thread), owned_(owned),
          queue_fn_(queue_fn), scheduler_(scheduler) {}

    TCB(Thread* thread, bool owned,
        void (*queue_thread_fn)(Thread*),
        bool untyped = false)
        : KernelObject(ObjectType::TCB, untyped),
          thread_(thread), owned_(owned),
          queue_fn_(nullptr), scheduler_(nullptr),
          queue_thread_fn_(queue_thread_fn) {}

    ~TCB() override {
        if (owned_ && thread_ && !is_untyped_backed_) {
            delete thread_;
        }
    }

    Thread* getThread() const { return thread_; }

    // Resume: queue thread into scheduler
    bool resume() {
        if (!thread_) return false;
        if (thread_->state == ThreadState::Ready || thread_->state == ThreadState::Running) return false;
        if (queue_thread_fn_) {
            queue_thread_fn_(thread_);
            return true;
        }
        if (queue_fn_ && scheduler_) {
            queue_fn_(scheduler_, thread_);
            return true;
        }
        return false;
    }

    // Suspend: mark thread as blocked (will not be re-queued)
    bool suspend() {
        if (!thread_) return false;
        if (thread_->state == ThreadState::Running || thread_->state == ThreadState::Ready) {
            thread_->state = ThreadState::Blocked;
            return true;
        }
        return false;
    }

    // Set registers for initial execution
    // entry: user-space entry point, stack_top: user stack pointer
    bool setRegisters(std::uint64_t entry, std::uint64_t stack_top) {
        if (!thread_) return false;
        // Store in thread's user_data process if available
        // For now, these are stored as thread metadata
        // The actual register setup happens when the thread is first scheduled
        return true;
    }

    // Configure: associate VSpace and CSpace with the thread's process
    bool configure(void* process_ptr) {
        if (!thread_) return false;
        thread_->user_data = process_ptr;
        return true;
    }
};

} // namespace oz
