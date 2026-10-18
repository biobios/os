#include "core/Notification.hpp"
#include "core/Thread.hpp"

namespace oz {

extern void (*g_unblock_thread_func)(Thread*);

Notification::Notification(bool untyped)
    : KernelObject(ObjectType::Notification, untyped) {}

Notification::~Notification() {
    // Wake up all waiters on destruction
    while (Thread* t = pop_waiter()) {
        t->state = ThreadState::Ready;
        if (g_unblock_thread_func) {
            g_unblock_thread_func(t);
        }
    }
}

void Notification::signal(std::uint64_t badge) {
    value_ |= badge;
    Thread* t = pop_waiter();
    if (t) {
        t->state = ThreadState::Ready;
        if (g_unblock_thread_func) {
            g_unblock_thread_func(t);
        }
    }
}

std::uint64_t Notification::poll() {
    std::uint64_t v = value_;
    value_ = 0;
    return v;
}

void Notification::add_waiter(Thread* t) {
    if (!t) return;
    t->next_waiter = nullptr;
    if (waiter_tail_) {
        waiter_tail_->next_waiter = t;
        waiter_tail_ = t;
    } else {
        waiter_head_ = waiter_tail_ = t;
    }
}

Thread* Notification::pop_waiter() {
    if (!waiter_head_) return nullptr;
    Thread* t = waiter_head_;
    waiter_head_ = t->next_waiter;
    if (!waiter_head_) waiter_tail_ = nullptr;
    t->next_waiter = nullptr;
    return t;
}

} // namespace oz
