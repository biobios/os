#include "core/Channel.hpp"
#include "core/Thread.hpp"

namespace oz {

void ChannelEndpoint::add_waiter(Thread* thread) {
    thread->next_waiter = nullptr;
    if (waiter_tail) {
        waiter_tail->next_waiter = thread;
        waiter_tail = thread;
    } else {
        waiter_head = waiter_tail = thread;
    }
}

Thread* ChannelEndpoint::pop_waiter() {
    if (!waiter_head) return nullptr;
    Thread* t = waiter_head;
    waiter_head = t->next_waiter;
    if (!waiter_head) waiter_tail = nullptr;
    t->next_waiter = nullptr;
    return t;
}

} // namespace oz
