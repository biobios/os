#include "core/Baton.hpp"
#include "core/ExecutionContext.hpp"
#include "core/Thread.hpp"
#include "core/Notification.hpp"
#include "core/IRQHandler.hpp"
#include "core/Scheduler.hpp"
#include "core/CartesianTree.hpp"
#include "core/Kernel.hpp"

namespace oz {

Baton::~Baton() {
    Scheduler& sched = Kernel::instance().getScheduler();
    bool in_cartesian = (cartesian_parent != nullptr || cartesian_left != nullptr ||
                         cartesian_right != nullptr || sched.cartesian_root == this);
    if (in_cartesian) {
        cartesianPushDown(this);
        Baton* merged_child = cartesianZipMerge(cartesian_left, cartesian_right);
        if (cartesian_parent) {
            if (cartesian_parent->cartesian_left == this) {
                cartesian_parent->cartesian_left = merged_child;
            } else if (cartesian_parent->cartesian_right == this) {
                cartesian_parent->cartesian_right = merged_child;
            }
        }
        if (merged_child) {
            merged_child->cartesian_parent = cartesian_parent;
        }
        if (sched.cartesian_root == this) {
            sched.cartesian_root = merged_child;
        }
        resetCartesian();
    }

    if (sched.running_baton == this) {
        sched.running_baton = caller_baton;
    }

    if (caller_baton) {
        caller_baton->callee_baton = callee_baton;
    }
    if (callee_baton) {
        callee_baton->caller_baton = caller_baton;
    }

    if (thread) {
        auto* t = static_cast<Thread*>(thread);
        if (t->own_baton == this) t->own_baton = nullptr;
        if (t->borrowed_baton == this) t->borrowed_baton = nullptr;
        if (thread->baton == this) {
            thread->baton = nullptr;
        }
    }
    if (bound_irq_handler && bound_irq_handler->assigned_irq_baton == this) {
        bound_irq_handler->assigned_irq_baton = nullptr;
    }
    bound_irq_handler = nullptr;
    thread = nullptr;
    caller_baton = nullptr;
    callee_baton = nullptr;
    tail_baton = nullptr;
    sub_min_baton = nullptr;
    caller_context = nullptr;
    bound_notif = nullptr;
    state = BatonState::READY;
}

bool Baton::is_runnable() const {
    return thread != nullptr && thread->baton == this && thread->state == ThreadState::Ready && !thread->is_blocked;
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
