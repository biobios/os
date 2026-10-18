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

public:
    explicit Notification(bool untyped = false);
    ~Notification() override;

    // Signal this notification with a badge bitmask (OR-combined)
    void signal(std::uint64_t badge);

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
    bool has_waiters() const { return waiter_head_ != nullptr; }
};

} // namespace oz
