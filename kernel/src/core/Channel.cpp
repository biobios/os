#include "core/Channel.hpp"
#include "core/Thread.hpp"
#include "core/Notification.hpp"

namespace oz {

extern void (*g_unblock_thread_func)(Thread*);

ChannelEndpoint::~ChannelEndpoint() {
    unbind_notification();
    if (peer) {
        peer->on_peer_closed();
    }
    while (Thread* t = pop_recv_waiter()) {
        t->state = ThreadState::Ready;
        if (g_unblock_thread_func) g_unblock_thread_func(t);
    }
    while (Thread* t = pop_send_waiter()) {
        t->state = ThreadState::Ready;
        if (g_unblock_thread_func) g_unblock_thread_func(t);
    }
}

void ChannelEndpoint::on_peer_closed() {
    peer = nullptr;
    if (bound_notification_) {
        bound_notification_->signal(bound_badge_);
    }
    while (Thread* t = pop_recv_waiter()) {
        t->state = ThreadState::Ready;
        if (g_unblock_thread_func) g_unblock_thread_func(t);
    }
    while (Thread* t = pop_send_waiter()) {
        t->state = ThreadState::Ready;
        if (g_unblock_thread_func) g_unblock_thread_func(t);
    }
}

void ChannelEndpoint::bind_notification(Notification* notif, std::uint64_t badge) {
    unbind_notification();
    if (notif) {
        notif->add_ref();
        bound_notification_ = notif;
        bound_badge_ = badge;
    }
}

void ChannelEndpoint::unbind_notification() {
    if (bound_notification_) {
        bound_notification_->release();
        bound_notification_ = nullptr;
        bound_badge_ = 0;
    }
}

void ChannelEndpoint::add_recv_waiter(Thread* thread) {
    thread->next_waiter = nullptr;
    if (recv_waiter_tail) {
        recv_waiter_tail->next_waiter = thread;
        recv_waiter_tail = thread;
    } else {
        recv_waiter_head = recv_waiter_tail = thread;
    }
}

Thread* ChannelEndpoint::pop_recv_waiter() {
    if (!recv_waiter_head) return nullptr;
    Thread* t = recv_waiter_head;
    recv_waiter_head = t->next_waiter;
    if (!recv_waiter_head) recv_waiter_tail = nullptr;
    t->next_waiter = nullptr;
    return t;
}

void ChannelEndpoint::add_send_waiter(Thread* thread) {
    thread->next_waiter = nullptr;
    if (send_waiter_tail) {
        send_waiter_tail->next_waiter = thread;
        send_waiter_tail = thread;
    } else {
        send_waiter_head = send_waiter_tail = thread;
    }
}

Thread* ChannelEndpoint::pop_send_waiter() {
    if (!send_waiter_head) return nullptr;
    Thread* t = send_waiter_head;
    send_waiter_head = t->next_waiter;
    if (!send_waiter_head) send_waiter_tail = nullptr;
    t->next_waiter = nullptr;
    return t;
}

} // namespace oz
