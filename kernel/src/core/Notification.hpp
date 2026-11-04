#pragma once
#include "core/Capability.hpp"
#include <cstdint>

namespace oz {

class Thread;

class Notification : public KernelObject {
private:
    std::uint64_t value_{0};
    Thread* waiter_head_{nullptr};
    Thread* waiter_tail_{nullptr};
    Thread* last_waiter_{nullptr};

    // Hierarchical Radix-64 Notification fields
    Notification* parent_{nullptr};
    std::uint64_t parent_badge_{0};
    std::uint32_t depth_{1};

    // Intrusive child list for safe teardown
    Notification* first_child_{nullptr};
    Notification* next_sibling_{nullptr};
    Notification* prev_sibling_{nullptr};

    // Global registry intrusive list
    Notification* reg_prev_{nullptr};
    Notification* reg_next_{nullptr};

public:
    explicit Notification(bool untyped = false);
    ~Notification() override;

    void destroy() override;

    // Signal this notification with a badge bitmask (OR-combined)
    // Returns the thread (if any) that was unblocked from waiting on this notification.
    Thread* signal(std::uint64_t badge);

    // Non-blocking poll: returns accumulated bits and resets value_ to 0
    std::uint64_t poll();

    bool has_signals() const { return value_ != 0; }
    std::uint64_t get_and_clear_value() {
        std::uint64_t v = value_;
        value_ = 0;
        return v;
    }

    // Waiter queue management
    void add_waiter(Thread* t);
    Thread* pop_waiter();
    bool remove_waiter(Thread* t);
    bool has_waiters() const { return waiter_head_ != nullptr; }

    friend void remove_thread_from_all_notifications(Thread* t);

    // Multi-stage Notification methods
    Notification* get_parent() const { return parent_; }
    std::uint64_t get_parent_badge() const { return parent_badge_; }
    std::uint32_t get_depth() const { return depth_; }

    int bind_parent(Notification* parent, std::uint64_t badge);
    int unbind_parent();
    void teardown();

    std::uint32_t get_subtree_height() const;
    void update_depths(std::uint32_t new_depth);

private:
    void add_child_internal(Notification* child);
    void remove_child_internal(Notification* child);
};

void remove_thread_from_all_notifications(Thread* t);

} // namespace oz
