#pragma once
#include "core/Capability.hpp"
#include "core/Channel.hpp"
#include "core/Notification.hpp"
#include "core/ExecutionContext.hpp"
#include "core/Thread.hpp"
#include "core/Baton.hpp"
#include "core/IRQDispatch.hpp"
#include <cstdint>

namespace oz {

// IRQHandler: capability-based interrupt handler object
// Associates an IRQ vector with a Notification or channel endpoint.
// When the interrupt fires, the kernel signals the Notification (pure notification,
// no CPU time theft) and temporarily masks the IRQ line until acknowledged.
class IRQHandler : public KernelObject {
private:
    std::uint8_t irq_vector_;
    ChannelEndpoint* notification_ep_{nullptr};  // Endpoint to send notification to (legacy)
    Notification* notif_{nullptr};               // Bound Notification object
    std::uint64_t badge_{0};                     // Badge to signal on interrupt
    ExecutionContext* handler_thread_{nullptr};  // Bound IRQ handler thread for Baton Model
    bool acknowledged_{true};                    // Whether the last interrupt was acknowledged
    bool is_masked_{false};                      // Temporary mask to prevent interrupt storms
    bool has_pending_{false};                    // Pending edge-triggered IRQ arrived while masked
    bool is_registered_{false};

    // Function pointers for interrupt management (set by Kernel.hpp)
    void (*register_fn_)(std::uint8_t vector, IRQHandler* handler);
    void (*unregister_fn_)(std::uint8_t vector, IRQHandler* handler);

public:
    IRQHandler(std::uint8_t vector,
               void (*register_fn)(std::uint8_t, IRQHandler*) = nullptr,
               void (*unregister_fn)(std::uint8_t, IRQHandler*) = nullptr,
               void (*eoi_fn)() = nullptr)
        : KernelObject(ObjectType::IRQHandler),
          irq_vector_(vector), notification_ep_(nullptr),
          notif_(nullptr), badge_(0),
          acknowledged_(true), is_masked_(false), has_pending_(false), is_registered_(false),
          register_fn_(register_fn), unregister_fn_(unregister_fn) {
        (void)eoi_fn;
    }

    ~IRQHandler() override {
        unregisterHandler();
    }

    std::uint8_t vector() const { return irq_vector_; }
    bool isRegistered() const { return is_registered_; }
    bool isMasked() const { return is_masked_; }
    bool hasPending() const { return has_pending_; }
    void setPending(bool pending = true) { has_pending_ = pending; }
    void clearPending() { has_pending_ = false; }

    void mask() {
        is_masked_ = true;
    }

    void unmask() {
        is_masked_ = false;
    }

    // Set the notification endpoint (legacy ChannelEndpoint)
    bool setNotificationEndpoint(ChannelEndpoint* ep) {
        notification_ep_ = ep;
        return true;
    }

    // Bind a Notification object and badge (Scheduling Context Donation pure model)
    bool setNotification(Notification* notif, std::uint64_t badge) {
        notif_ = notif;
        badge_ = badge;
        return true;
    }

    void setHandlerThread(ExecutionContext* thread) {
        handler_thread_ = thread;
    }

    ExecutionContext* getHandlerThread() const {
        return handler_thread_;
    }

    // Called by the kernel interrupt dispatcher when this IRQ fires
    void onInterrupt() {
        if (is_masked_) return;
        is_masked_ = true; // Temporary mask to avoid interrupt storm
        acknowledged_ = false;

        // 1. Scheduling Context Donation model: pure notification signal
        if (notif_) {
            notif_->signal(badge_ ? badge_ : (1ULL << (irq_vector_ % 64)));
        }

        // 2. ChannelEndpoint waiter support
        if (notification_ep_ && !notification_ep_->is_peer_closed()) {
            ChannelEndpoint* peer = notification_ep_->get_peer();
            if (peer) {
                peer->set_notification(irq_vector_);
                Thread* waiter = peer->pop_recv_waiter();
                if (waiter) {
                    waiter->is_blocked = false;
                    waiter->state = ThreadState::Ready;
                    if (waiter->baton) {
                        waiter->baton->notify_if_runnable();
                    }
                    if (waiter->sched_notif && waiter->sched_badge != 0) {
                        waiter->sched_notif->signal(waiter->sched_badge);
                    }
                }
            }
        }
    }

    // Acknowledge the interrupt (user-space calls this after handling)
    bool acknowledge() {
        acknowledged_ = true;
        is_masked_ = false; // Unmask IRQ line
        return true;
    }

    // Register this handler with the kernel interrupt system
    bool registerHandler() {
        if (is_registered_) return true;
        if (register_fn_) {
            register_fn_(irq_vector_, this);
        } else {
            oz::irq::registerHandler(irq_vector_, this);
        }
        is_registered_ = true;
        return true;
    }

    // Unregister this handler from the kernel interrupt system
    bool unregisterHandler() {
        if (!is_registered_) return false;
        if (unregister_fn_) {
            unregister_fn_(irq_vector_, this);
        } else {
            oz::irq::unregisterHandler(irq_vector_, this);
        }
        is_registered_ = false;
        return true;
    }
};

} // namespace oz
