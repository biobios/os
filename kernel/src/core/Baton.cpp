#include "core/Baton.hpp"
#include "core/ExecutionContext.hpp"
#include "core/Notification.hpp"

namespace oz {

Baton::~Baton() {
    if (thread && thread->baton == this) {
        thread->baton = nullptr;
    }
    thread = nullptr;
    caller_context = nullptr;
    bound_notif = nullptr;
}

bool Baton::is_runnable() const {
    return thread != nullptr && thread->state == ThreadState::Ready && !thread->is_blocked;
}

void Baton::notify_if_runnable() {
    if (is_irq_baton) {
        // IRQハンドラのバトンは Ready による通知を行わない（枯渇時のみ明示的にシグナルされる）
        return;
    }
    if (is_runnable() && bound_notif && bound_badge != 0) {
        bound_notif->signal(bound_badge);
    }
}

} // namespace oz
