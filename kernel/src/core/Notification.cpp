#include "core/Notification.hpp"
#include "core/Thread.hpp"
#include "core/Baton.hpp"
#include "core/Channel.hpp"
#include "core/IRQHandler.hpp"
#include "core/KernelUtils.hpp"

namespace oz {

static Notification* s_notification_list_head = nullptr;

Notification::Notification(bool untyped)
    : KernelObject(ObjectType::Notification, untyped) {
    reg_prev_ = nullptr;
    reg_next_ = s_notification_list_head;
    if (s_notification_list_head) {
        s_notification_list_head->reg_prev_ = this;
    }
    s_notification_list_head = this;
}

Notification::~Notification() {
    if (reg_prev_) {
        reg_prev_->reg_next_ = reg_next_;
    } else if (s_notification_list_head == this) {
        s_notification_list_head = reg_next_;
    }
    if (reg_next_) {
        reg_next_->reg_prev_ = reg_prev_;
    }
    reg_prev_ = nullptr;
    reg_next_ = nullptr;

    teardown();
    last_waiter_ = nullptr;
    // Wake up all waiters on destruction
    while (Thread* t = pop_waiter()) {
        t->state = ThreadState::Ready;
        oz::unblock_thread(t);
    }

    // 逆引き強制アンバインド
    for_each_registered_thread([this](Thread* t) {
        if (!t) return;
        if (t->exit_notif == this) {
            t->exit_notif = nullptr;
            t->exit_badge = 0;
        }
        if (t->sched_notif == this) {
            t->sched_notif = nullptr;
            t->sched_badge = 0;
        }
        if (t->bound_notification == this) {
            t->bound_notification = nullptr;
        }
        if (t->own_baton && t->own_baton->bound_notif == this) {
            t->own_baton->bound_notif = nullptr;
            t->own_baton->bound_badge = 0;
        }
        if (t->borrowed_baton && t->borrowed_baton->bound_notif == this) {
            t->borrowed_baton->bound_notif = nullptr;
            t->borrowed_baton->bound_badge = 0;
        }
    });

    unbind_notification_from_all_channels(this);
    unbind_notification_from_all_batons(this);

    for (std::size_t i = 0; i < 256; ++i) {
        if (g_irq_handlers[i].notif_ == this) {
            g_irq_handlers[i].notif_ = nullptr;
            g_irq_handlers[i].badge_ = 0;
        }
    }
}

void Notification::destroy() {
    teardown();
    if (!is_untyped_backed_) {
        delete this;
    } else {
        this->~Notification();
    }
}

void Notification::teardown() {
    // 1. Unbind from parent if bound
    if (parent_) {
        parent_->remove_child_internal(this);
        parent_ = nullptr;
        parent_badge_ = 0;
        depth_ = 1;
    }

    // 2. Unbind all children
    Notification* child = first_child_;
    while (child) {
        Notification* next = child->next_sibling_;
        child->parent_ = nullptr;
        child->parent_badge_ = 0;
        child->prev_sibling_ = nullptr;
        child->next_sibling_ = nullptr;
        child->update_depths(1);
        child = next;
    }
    first_child_ = nullptr;
}

void Notification::add_child_internal(Notification* child) {
    if (!child) return;
    child->next_sibling_ = first_child_;
    child->prev_sibling_ = nullptr;
    if (first_child_) {
        first_child_->prev_sibling_ = child;
    }
    first_child_ = child;
}

void Notification::remove_child_internal(Notification* child) {
    if (!child) return;
    if (child->prev_sibling_) {
        child->prev_sibling_->next_sibling_ = child->next_sibling_;
    } else if (first_child_ == child) {
        first_child_ = child->next_sibling_;
    }
    if (child->next_sibling_) {
        child->next_sibling_->prev_sibling_ = child->prev_sibling_;
    }
    child->prev_sibling_ = nullptr;
    child->next_sibling_ = nullptr;
}

std::uint32_t Notification::get_subtree_height() const {
    std::uint32_t max_child_h = 0;
    for (Notification* ch = first_child_; ch != nullptr; ch = ch->next_sibling_) {
        std::uint32_t ch_h = ch->get_subtree_height();
        if (ch_h > max_child_h) {
            max_child_h = ch_h;
        }
    }
    return 1 + max_child_h;
}

void Notification::update_depths(std::uint32_t new_depth) {
    depth_ = new_depth;
    for (Notification* ch = first_child_; ch != nullptr; ch = ch->next_sibling_) {
        ch->update_depths(new_depth + 1);
    }
}

int Notification::bind_parent(Notification* parent, std::uint64_t badge) {
    if (!parent) return static_cast<int>(SYS_ERR_INVALID);

    // 1. One-hot badge invariant: strictly 1 bit set ((badge != 0) && ((badge & (badge - 1)) == 0))
    if (badge == 0 || (badge & (badge - 1)) != 0) {
        return static_cast<int>(SYS_ERR_INVALID);
    }

    // 2. Acyclic invariant: cycle detection
    if (parent == this) {
        return static_cast<int>(SYS_ERR_DEADLK);
    }

    std::uint32_t parent_depth = 0;
    for (Notification* p = parent; p != nullptr; p = p->parent_) {
        parent_depth++;
        if (p == this) {
            return static_cast<int>(SYS_ERR_DEADLK);
        }
        if (parent_depth > 4) {
            return static_cast<int>(SYS_ERR_INVALID);
        }
    }

    // 3. Max depth limit: tree depth (parent ancestor depth + child subtree height) <= 4
    if (parent_depth + get_subtree_height() > 4) {
        return static_cast<int>(SYS_ERR_INVALID);
    }

    // If already bound to a parent, unbind first
    if (parent_) {
        unbind_parent();
    }

    parent_ = parent;
    parent_badge_ = badge;
    parent_->add_child_internal(this);
    update_depths(parent_->depth_ + 1);

    return 0;
}

int Notification::unbind_parent() {
    if (!parent_) {
        return 0;
    }
    parent_->remove_child_internal(this);
    parent_ = nullptr;
    parent_badge_ = 0;
    update_depths(1);
    return 0;
}

Thread* Notification::signal(std::uint64_t badge) {
    bool was_zero = (value_ == 0);
    value_ |= badge;
    Thread* unblocked = pop_waiter();
    if (unblocked) {
        unblocked->state = ThreadState::Ready;
        oz::unblock_thread(unblocked);
    } else if (last_waiter_ && !last_waiter_->is_dead && !last_waiter_->is_blocked) {
        unblocked = last_waiter_;
    }

    // Cascade notification to parent if bound (rising edge: only if 0 -> non-zero)
    if (was_zero && value_ != 0 && parent_ != nullptr) {
        Thread* parent_unblocked = parent_->signal(parent_badge_);
        if (!unblocked) {
            unblocked = parent_unblocked;
        }
    }

    return unblocked;
}

std::uint64_t Notification::poll() {
    std::uint64_t v = value_;
    value_ = 0;
    return v;
}

void Notification::add_waiter(Thread* t) {
    if (!t) return;
    last_waiter_ = t;
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

bool Notification::remove_waiter(Thread* t) {
    if (!t) return false;
    if (last_waiter_ == t) {
        last_waiter_ = nullptr;
    }
    if (!waiter_head_) return false;
    if (waiter_head_ == t) {
        waiter_head_ = t->next_waiter;
        if (!waiter_head_) waiter_tail_ = nullptr;
        t->next_waiter = nullptr;
        return true;
    }
    Thread* curr = waiter_head_;
    while (curr->next_waiter && curr->next_waiter != t) {
        curr = curr->next_waiter;
    }
    if (curr->next_waiter == t) {
        curr->next_waiter = t->next_waiter;
        if (waiter_tail_ == t) {
            waiter_tail_ = curr;
        }
        t->next_waiter = nullptr;
        return true;
    }
    return false;
}

void remove_thread_from_all_notifications(Thread* t) {
    if (!t) return;
    for (Notification* n = s_notification_list_head; n != nullptr; n = n->reg_next_) {
        n->remove_waiter(t);
    }
}

} // namespace oz
