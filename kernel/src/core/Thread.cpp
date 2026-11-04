#include "core/Thread.hpp"
#include "core/Capability.hpp"
#include "core/Channel.hpp"
#include "core/Notification.hpp"
#include "core/Baton.hpp"
#include "core/IRQHandler.hpp"
#include "core/Scheduler.hpp"
#include "core/CartesianTree.hpp"
#include "core/KernelUtils.hpp"
#include "utils/utils.hpp"

namespace oz {

Thread* g_current_irq_thread = nullptr;

static constexpr std::size_t MAX_GLOBAL_THREADS = 512;
static Thread* s_thread_registry[MAX_GLOBAL_THREADS]{};

void register_thread(Thread* t) {
    if (!t) return;
    for (std::size_t i = 0; i < MAX_GLOBAL_THREADS; ++i) {
        if (!s_thread_registry[i] || s_thread_registry[i] == t) {
            s_thread_registry[i] = t;
            return;
        }
    }
}

void for_each_registered_thread_impl(void (*callback)(Thread*, void*), void* data) {
    if (!callback) return;
    for (std::size_t i = 0; i < MAX_GLOBAL_THREADS; ++i) {
        if (Thread* t = s_thread_registry[i]) {
            callback(t, data);
        }
    }
}

void unregister_thread(Thread* t) {
    if (!t) return;
    for (std::size_t i = 0; i < MAX_GLOBAL_THREADS; ++i) {
        if (s_thread_registry[i] == t) {
            s_thread_registry[i] = nullptr;
            return;
        }
    }
}

Thread* get_thread_by_id(std::uint64_t id) {
    if (id == 0) return nullptr;
    for (std::size_t i = 0; i < MAX_GLOBAL_THREADS; ++i) {
        Thread* t = s_thread_registry[i];
        if (t && t->id == id && !t->is_dead && t->state != ThreadState::Dead) {
            return t;
        }
    }
    return nullptr;
}

void dump_threads() {
    dprint("[Sched Dump] Registered threads:\n\r");
    for (std::size_t i = 0; i < MAX_GLOBAL_THREADS; ++i) {
        Thread* t = s_thread_registry[i];
        if (t && t->id != 0 && !t->is_dead) {
            dprint("  [TID ");
            // Print decimal TID
            char buf[16];
            int pos = 0;
            std::uint64_t val = t->id;
            if (val == 0) buf[pos++] = '0';
            else {
                char tmp[16];
                int tpos = 0;
                while (val > 0) { tmp[tpos++] = '0' + (val % 10); val /= 10; }
                while (tpos > 0) buf[pos++] = tmp[--tpos];
            }
            buf[pos] = '\0';
            dprint(buf);
            dprint("] state=");
            switch (t->state) {
                case ThreadState::Ready: dprint("Ready"); break;
                case ThreadState::Running: dprint("Running"); break;
                case ThreadState::Blocked: dprint("Blocked"); break;
                case ThreadState::BlockedOnSend: dprint("BlockedOnSend"); break;
                case ThreadState::BlockedOnRecv: dprint("BlockedOnRecv"); break;
                case ThreadState::BlockedOnReply: dprint("BlockedOnReply"); break;
                case ThreadState::Dead: dprint("Dead"); break;
                default: dprint("Unknown"); break;
            }
            dprint(" is_blocked=");
            dprint(t->is_blocked ? "1" : "0");
            dprint("\n\r");
        }
    }
}

Thread::Thread(std::uint64_t id, void* stack, std::uint64_t size, PageBlock<> block, std::uint8_t level, PhysicalAddress<PageTable> pml4_phys)
    : ExecutionContext(id, stack, size, block, level, pml4_phys) {
    register_thread(this);
}

Thread::~Thread() {
    cancel_ipc();

    if (this->pending_server) {
        if (this->pending_server->reply_target == this) {
            this->pending_server->reply_target = nullptr;
        }
        this->pending_server = nullptr;
    }
    if (this->reply_target) {
        if (this->reply_target->pending_server == this) {
            this->reply_target->pending_server = nullptr;
        }
        this->reply_target = nullptr;
    }

    remove_thread_from_all_notifications(this);

    if (this->is_irq_handling()) {
        Baton* irq_b = this->borrowed_baton;
        if (irq_b) {
            irq_b->thread = nullptr;
            irq_b->state = BatonState::READY;
            irq_b->remaining = 0;
            this->borrowed_baton = nullptr;
        }
        if (g_current_irq_thread == this) {
            g_current_irq_thread = this->next_nested_irq;
            this->next_nested_irq = nullptr;
        }
    }

    if (g_scheduler_ptr) {
        Baton* b = this->get_active_baton();
        if (!b) b = this->baton;
        if (b && g_scheduler_ptr->running_baton == b) {
            cartesianPopLeaf(g_scheduler_ptr->cartesian_root, b);
            b->state = BatonState::READY;
            b->is_active = false;
            ExecutionContext* caller = b->caller_context;
            Baton* caller_b = b->caller_baton;
            b->caller_baton = nullptr;
            b->caller_context = nullptr;
            if (caller_b) {
                caller_b->callee_baton = nullptr;
                caller_b->state = BatonState::RUNNING;
                cartesianPushDown(caller_b);
                g_scheduler_ptr->running_baton = caller_b;
                if (!caller) caller = caller_b->thread;
            } else {
                g_scheduler_ptr->running_baton = nullptr;
            }
            if (caller && caller != this) {
                caller->sched_reason = SchedReason::ORPHANED;
            }
        }
    }

    if (this->own_baton) {
        if (this->own_baton->thread == this) {
            this->own_baton->thread = nullptr;
            this->own_baton->is_active = false;
        }
        this->own_baton = nullptr;
    }
    if (this->borrowed_baton) {
        if (this->borrowed_baton->thread == this) {
            this->borrowed_baton->thread = nullptr;
            this->borrowed_baton->is_active = false;
        }
        this->borrowed_baton = nullptr;
    }
    if (this->baton) {
        if (this->baton->thread == this) {
            this->baton->thread = nullptr;
            this->baton->is_active = false;
        }
        this->baton = nullptr;
    }

    unbind_thread_from_all_batons(this);

    if (this->bound_irq_handler) {
        if (this->bound_irq_handler->handler_thread == this) {
            this->bound_irq_handler->handler_thread = nullptr;
            this->bound_irq_handler->assigned_irq_baton = nullptr;
        }
        this->bound_irq_handler = nullptr;
    }
    unregister_thread(this);
    this->is_dead = true;
    this->state = ThreadState::Dead;
}

bool Thread::cancel_ipc() {
    if (state != ThreadState::BlockedOnRecv && state != ThreadState::BlockedOnSend &&
        state != ThreadState::BlockedOnCall && state != ThreadState::BlockedOnReply) {
        return false;
    }
    if (this->pending_server) {
        if (this->pending_server->reply_target == this) {
            this->pending_server->reply_target = nullptr;
        }
        this->pending_server = nullptr;
    }
    if (this->reply_target) {
        if (this->reply_target->pending_server == this) {
            this->reply_target->pending_server = nullptr;
        }
        this->reply_target = nullptr;
    }
    if (blocking_ep) {
        if (state == ThreadState::BlockedOnRecv) {
            blocking_ep->remove_recv_waiter(this);
        } else if (state == ThreadState::BlockedOnSend || state == ThreadState::BlockedOnCall) {
            blocking_ep->remove_send_waiter(this);
            send_msg = InternalIPCMessage{};
        }
        blocking_ep = nullptr;
    }
    ipc_cancelled = true;
    state = ThreadState::Ready;
    oz::unblock_thread(this);
    return true;
}

} // namespace oz

extern "C" [[noreturn]] __attribute__((naked)) void oz::load_context_noreturn(void* next_rsp) {
    __asm__ volatile(
        "movq %rdi, %rsp\n"
        "popq %r15\n"
        "popq %r14\n"
        "popq %r13\n"
        "popq %r12\n"
        "popq %rbp\n"
        "popq %rbx\n"
        "ret\n"
    );
}

extern "C" __attribute__((naked)) void oz::switch_context(void** old_rsp, void* new_rsp) {
    __asm__ volatile(
        // Save current thread's callee-saved registers
        "pushq %rbx\n"
        "pushq %rbp\n"
        "pushq %r12\n"
        "pushq %r13\n"
        "pushq %r14\n"
        "pushq %r15\n"
        
        // Save current rsp to *old_rsp
        "movq %rsp, (%rdi)\n"
        
        // Load new rsp from new_rsp
        "movq %rsi, %rsp\n"
        
        // Restore next thread's callee-saved registers
        "popq %r15\n"
        "popq %r14\n"
        "popq %r13\n"
        "popq %r12\n"
        "popq %rbp\n"
        "popq %rbx\n"
        
        // Return to next thread's execution point
        "ret\n"
    );
}

extern "C" __attribute__((naked)) void oz::thread_stub() {
    __asm__ volatile(
        "sti\n"             // Enable interrupts for the new thread
        "movq %rbx, %rdi\n" // Move arg to first argument register
        "call *%r12\n"      // Call entry function
        // When the thread entry function returns, terminate the thread
        "movq %r14, %rdi\n" // Pass scheduler pointer as argument
        "call *%r13\n"
        "1: hlt\n"
        "jmp 1b\n"
    );
}
