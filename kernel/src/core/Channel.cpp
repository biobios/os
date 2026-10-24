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

        // If peer is already closed or has senders waiting, signal immediately!
        if (is_peer_closed() || (peer && peer->has_send_waiters())) {
            bound_notification_->signal(bound_badge_);
        }
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

namespace {

bool remove_waiter_node(Thread*& head, Thread*& tail, Thread* thread) {
    if (!head || !thread) return false;
    if (head == thread) {
        head = thread->next_waiter;
        if (!head) tail = nullptr;
        thread->next_waiter = nullptr;
        return true;
    }
    Thread* curr = head;
    while (curr->next_waiter && curr->next_waiter != thread) {
        curr = curr->next_waiter;
    }
    if (curr->next_waiter == thread) {
        curr->next_waiter = thread->next_waiter;
        if (tail == thread) {
            tail = curr;
        }
        thread->next_waiter = nullptr;
        return true;
    }
    return false;
}

} // namespace

bool ChannelEndpoint::remove_recv_waiter(Thread* thread) {
    return remove_waiter_node(recv_waiter_head, recv_waiter_tail, thread);
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

bool ChannelEndpoint::remove_send_waiter(Thread* thread) {
    return remove_waiter_node(send_waiter_head, send_waiter_tail, thread);
}

} // namespace oz
