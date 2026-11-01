#pragma once
#include "core/Capability.hpp"
#include "core/Thread.hpp"
#include <cstdint>

#include "core/VSpace.hpp"
#include "core/Notification.hpp"
#include "utils/utils.hpp"

void dprint_hex(std::uint64_t);
void dprint_dec(std::uint64_t);

namespace oz {

class CSpace;

extern "C" void enter_user_mode(std::uint64_t rip, std::uint64_t user_rsp, std::uint64_t rflags);

inline void user_thread_entry(void* arg) {
    Thread* t = static_cast<Thread*>(arg);
    enter_user_mode(t->user_rip, t->user_rsp, 0x202);
}

class TCB : public KernelObject, public PhysicalAddressProvider {
private:
    Thread* thread_;
    bool owned_;  // If true, TCB owns the thread and will schedule cleanup on destruction

public:
    TCB(Thread* thread, bool owned,
        void (*queue_fn)(void*, Thread*) = nullptr, void* scheduler = nullptr,
        bool untyped = false)
        : KernelObject(ObjectType::TCB, untyped),
          thread_(thread), owned_(owned) {
        (void)queue_fn; (void)scheduler;
    }

    TCB(Thread* thread, bool owned,
        void (*queue_thread_fn)(Thread*),
        bool untyped = false)
        : KernelObject(ObjectType::TCB, untyped),
          thread_(thread), owned_(owned) {
        (void)queue_thread_fn;
    }

    ~TCB() override {
        if (thread_) {
            thread_->state = ThreadState::Dead;
        }
        if (owned_ && thread_ && !is_untyped_backed_) {
            delete thread_;
        }
    }

    Thread* getThread() const { return thread_; }
    std::uint64_t getThreadId() const { return thread_ ? thread_->id : 0; }

    void setFaultEndpoint(ChannelEndpoint* ep) {
        if (thread_) {
            thread_->fault_ep = ep;
        }
    }

    ChannelEndpoint* getFaultEndpoint() const {
        return thread_ ? thread_->fault_ep : nullptr;
    }

    void setExitNotification(Notification* notif, std::uint64_t badge) {
        if (thread_) {
            thread_->exit_notif = notif;
            thread_->exit_badge = badge;
        }
    }

    Notification* getExitNotification() const {
        return thread_ ? thread_->exit_notif : nullptr;
    }

    std::uint64_t getExitBadge() const {
        return thread_ ? thread_->exit_badge : 0;
    }

    void setSchedNotification(Notification* notif, std::uint64_t badge) {
        if (thread_) {
            thread_->sched_notif = notif;
            thread_->bound_notification = notif;
            thread_->sched_badge = badge;
        }
    }

    Notification* getSchedNotification() const {
        return thread_ ? thread_->sched_notif : nullptr;
    }

    std::uint64_t getSchedBadge() const {
        return thread_ ? thread_->sched_badge : 0;
    }

    // Resume: unblock thread and signal sched notification
    bool resume() {
        if (!thread_) return false;
        if (!thread_->is_blocked && thread_->state == ThreadState::Running) return false;
        thread_->is_blocked = false;
        thread_->state = ThreadState::Ready;
        if (thread_->sched_notif && thread_->sched_badge != 0) {
            thread_->sched_notif->signal(thread_->sched_badge);
        }
        return true;
    }

    // Suspend: mark thread as blocked (will not be re-queued)
    bool suspend() {
        if (!thread_) return false;
        thread_->is_blocked = true;
        thread_->state = ThreadState::Blocked;
        return true;
    }

    // Cancel blocked IPC and revoke pending replies
    bool cancel_ipc() {
        if (!thread_) return false;
        return thread_->cancel_ipc();
    }

    // Get accumulated CPU cycles (O(1))
    std::uint64_t get_total_cpu_cycles() const {
        if (!thread_) return 0;
        return thread_->total_cpu_cycles;
    }

    // Set registers for initial execution
    // entry: user-space entry point, stack_top: user stack pointer
    bool setRegisters(std::uint64_t entry, std::uint64_t stack_top) {
        if (!thread_) return false;
        thread_->user_rip = entry;
        thread_->user_rsp = stack_top;
        return true;
    }

    // Configure: associate VSpace, CSpace, and execution context
    bool configure(CSpace* cspace, VSpace* vspace, std::uint64_t rip, std::uint64_t user_rsp) {
        if (!thread_) return false;
        thread_->cspace = cspace;
        if (vspace && vspace->getPhysicalPML4() != 0) {
            thread_->pml4_phys = createPhysicalAddress<PageTable>(vspace->getPhysicalPML4());
        }
        thread_->user_rip = rip;
        thread_->user_rsp = user_rsp;
        thread_->state = ThreadState::Blocked;

        // Set up stack for switch_context
        std::uint64_t rsp = reinterpret_cast<std::uint64_t>(thread_->stack_base) + thread_->stack_size;
        rsp &= ~15ULL; // 16-byte align

        rsp -= sizeof(ThreadContext);
        thread_->rsp = reinterpret_cast<void*>(rsp);

        ThreadContext* ctx = reinterpret_cast<ThreadContext*>(rsp);
        ctx->rip = reinterpret_cast<std::uint64_t>(&thread_stub);
        ctx->r12 = reinterpret_cast<std::uint64_t>(&user_thread_entry);
        ctx->rbx = reinterpret_cast<std::uint64_t>(thread_);
        ctx->r13 = 0;
        ctx->r14 = 0;
        ctx->r15 = 0;
        ctx->rbp = 0;

        return true;
    }

    bool configure(void* process_ptr) {
        if (!thread_) return false;
        thread_->user_data = process_ptr;
        return true;
    }
};

} // namespace oz
