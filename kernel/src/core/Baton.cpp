#include "core/Baton.hpp"
#include "core/ExecutionContext.hpp"
#include "core/Thread.hpp"
#include "core/Notification.hpp"
#include "core/IRQHandler.hpp"
#include "core/Scheduler.hpp"
#include "core/CartesianTree.hpp"
#include "core/Kernel.hpp"

namespace oz {

static Baton* s_baton_list_head = nullptr;

Baton::Baton(bool untyped)
    : KernelObject(ObjectType::Baton, untyped) {
    reg_prev_ = nullptr;
    reg_next_ = s_baton_list_head;
    if (s_baton_list_head) {
        s_baton_list_head->reg_prev_ = this;
    }
    s_baton_list_head = this;
}

Baton::~Baton() {
    if (reg_prev_) {
        reg_prev_->reg_next_ = reg_next_;
    } else if (s_baton_list_head == this) {
        s_baton_list_head = reg_next_;
    }
    if (reg_next_) {
        reg_next_->reg_prev_ = reg_prev_;
    }
    reg_prev_ = nullptr;
    reg_next_ = nullptr;

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
        if (caller_baton) {
            caller_baton->state = BatonState::RUNNING;
            ExecutionContext* caller = caller_context ? caller_context : caller_baton->thread;
            if (caller) {
                caller->sched_reason = SchedReason::ORPHANED;
            }
        } else {
            sched.cartesian_root = nullptr;
            oz::x86_64::stopLAPICTimer();
        }
    }

    if (caller_baton) {
        caller_baton->callee_baton = callee_baton;
    }
    if (callee_baton) {
        callee_baton->caller_baton = caller_baton;
    }

    // 紐付く Thread のクリア
    for_each_registered_thread([this](Thread* t) {
        if (!t) return;
        if (t->own_baton == this) t->own_baton = nullptr;
        if (t->borrowed_baton == this) t->borrowed_baton = nullptr;
        if (t->baton == this) t->baton = t->get_active_baton();
    });

    for (std::size_t i = 0; i < 256; ++i) {
        if (g_irq_handlers[i].assigned_irq_baton == this) {
            g_irq_handlers[i].assigned_irq_baton = nullptr;
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

void unbind_notification_from_all_batons(Notification* notif) {
    if (!notif) return;
    for (Baton* b = s_baton_list_head; b != nullptr; b = b->reg_next_) {
        if (b->bound_notif == notif) {
            b->bound_notif = nullptr;
            b->bound_badge = 0;
        }
    }
}

void unbind_thread_from_all_batons(ExecutionContext* ctx) {
    if (!ctx) return;
    for (Baton* b = s_baton_list_head; b != nullptr; b = b->reg_next_) {
        if (b->thread == ctx) {
            b->thread = nullptr;
            b->is_active = false;
        }
        if (b->caller_context == ctx) {
            b->caller_context = nullptr;
        }
    }
}

} // namespace oz
