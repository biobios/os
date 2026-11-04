#pragma once

#include "core/Kernel.hpp"
#include "core/Thread.hpp"
#include "core/Baton.hpp"
#include "core/Notification.hpp"

namespace oz {

inline void unblock_thread(Thread* t) {
    if (!t) return;
    t->is_blocked = false;
    t->state = ThreadState::Ready;
    if (t->baton) {
        t->baton->notify_if_runnable();
    }
    if (t->sched_notif && t->sched_badge != 0) {
        t->sched_notif->signal(t->sched_badge);
    }
}

inline ExecutionContext* get_current_context() {
    return Kernel::instance().getScheduler().getCurrentContext();
}

inline Thread* get_current_thread() {
    return Kernel::instance().getScheduler().getCurrentThread();
}

inline CSpace* get_current_cspace() {
    auto* ctx = get_current_context();
    return ctx ? ctx->cspace : nullptr;
}

inline void block_current_thread() {
    Kernel::instance().getScheduler().schedule();
}

inline void exit_current_thread() {
    Kernel::instance().getScheduler().exitThread();
}

} // namespace oz
