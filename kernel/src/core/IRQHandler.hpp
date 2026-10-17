#pragma once
#include "core/Capability.hpp"
#include "core/Channel.hpp"
#include <cstdint>

namespace oz {

// IRQHandler: capability-based interrupt handler object
// Associates an IRQ vector with a notification channel endpoint.
// When the interrupt fires, the kernel sends a notification message
// to the associated endpoint, waking up any waiting thread.
class IRQHandler : public KernelObject {
private:
    std::uint8_t irq_vector_;
    ChannelEndpoint* notification_ep_;  // Endpoint to send notification to (not owned)
    bool acknowledged_;  // Whether the last interrupt was acknowledged

    // Function pointers for interrupt management (set by Kernel.hpp)
    void (*register_fn_)(std::uint8_t vector, IRQHandler* handler);
    void (*eoi_fn_)();

public:
    IRQHandler(std::uint8_t vector,
               void (*register_fn)(std::uint8_t, IRQHandler*) = nullptr,
               void (*eoi_fn)() = nullptr)
        : KernelObject(ObjectType::IRQHandler),
          irq_vector_(vector), notification_ep_(nullptr),
          acknowledged_(true),
          register_fn_(register_fn), eoi_fn_(eoi_fn) {}

    std::uint8_t vector() const { return irq_vector_; }

    // Set the notification endpoint
    bool setNotificationEndpoint(ChannelEndpoint* ep) {
        notification_ep_ = ep;
        return true;
    }

    // Called by the kernel interrupt dispatcher when this IRQ fires
    // Returns the endpoint's peer waiter (if any) to be unblocked
    Thread* onInterrupt() {
        if (!notification_ep_ || notification_ep_->is_peer_closed()) return nullptr;
        acknowledged_ = false;

        ChannelEndpoint* peer = notification_ep_->get_peer();
        if (!peer) return nullptr;

        peer->set_notification(irq_vector_);
        return peer->pop_recv_waiter();
    }

    // Acknowledge the interrupt (user-space calls this after handling)
    bool acknowledge() {
        if (acknowledged_) return false;
        acknowledged_ = true;
        // LAPIC EOI is handled by the ISR stub, not here.
        // This method just resets the acknowledged flag for the next interrupt.
        return true;
    }

    // Register this handler with the kernel interrupt system
    bool registerHandler() {
        if (!register_fn_) return false;
        register_fn_(irq_vector_, this);
        return true;
    }
};

} // namespace oz
