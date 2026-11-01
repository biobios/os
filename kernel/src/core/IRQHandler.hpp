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
public:
    std::uint8_t vector{0};
    Thread* handler_thread{nullptr};             // Bound IRQ handler thread for Baton Model
    Baton* assigned_irq_baton{nullptr};          // irq_baton to lend on next interrupt
    bool is_masked{false};                       // Hardware line mask state
    bool pending{false};                         // Pending IRQ arrived while masked

private:
    ChannelEndpoint* notification_ep_{nullptr};  // Endpoint to send notification to (legacy)
    Notification* notif_{nullptr};               // Bound Notification object
    std::uint64_t badge_{0};                     // Badge to signal on interrupt
    bool acknowledged_{true};                    // Whether the last interrupt was acknowledged
    bool is_registered_{false};

    // Function pointers for interrupt management (set by Kernel.hpp)
    void (*register_fn_)(std::uint8_t vector, IRQHandler* handler);
    void (*unregister_fn_)(std::uint8_t vector, IRQHandler* handler);

public:
    IRQHandler(std::uint8_t vec,
               void (*register_fn)(std::uint8_t, IRQHandler*) = nullptr,
               void (*unregister_fn)(std::uint8_t, IRQHandler*) = nullptr,
               void (*eoi_fn)() = nullptr)
        : KernelObject(ObjectType::IRQHandler),
          vector(vec),
          handler_thread(nullptr), assigned_irq_baton(nullptr),
          is_masked(false), pending(false),
          notification_ep_(nullptr),
          notif_(nullptr), badge_(0),
          acknowledged_(true), is_registered_(false),
          register_fn_(register_fn), unregister_fn_(unregister_fn) {
        (void)eoi_fn;
    }

    ~IRQHandler() override {
        unregisterHandler();
        if (handler_thread && handler_thread->bound_irq_handler == this) {
            handler_thread->bound_irq_handler = nullptr;
        }
        handler_thread = nullptr;
        assigned_irq_baton = nullptr;
    }

    std::uint8_t get_vector() const { return vector; }
    bool isRegistered() const { return is_registered_; }
    bool isMasked() const { return is_masked; }
    bool hasPending() const { return pending; }
    void setPending(bool p = true) { pending = p; }
    void clearPending() { pending = false; }

    void mask() {
        is_masked = true;
    }

    void unmask() {
        is_masked = false;
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

    void setHandlerThread(Thread* thread) {
        handler_thread = thread;
    }

    Thread* getHandlerThread() const {
        return handler_thread;
    }

    // Called by the kernel interrupt dispatcher when this IRQ fires
    void onInterrupt() {
        if (is_masked) return;
        is_masked = true; // Temporary mask to avoid interrupt storm
        acknowledged_ = false;

        // 1. Scheduling Context Donation model: pure notification signal
        if (notif_) {
            notif_->signal(badge_ ? badge_ : (1ULL << (vector % 64)));
        }

        // 2. ChannelEndpoint waiter support
        if (notification_ep_ && !notification_ep_->is_peer_closed()) {
            ChannelEndpoint* peer = notification_ep_->get_peer();
            if (peer) {
                peer->set_notification(vector);
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
        is_masked = false; // Unmask IRQ line
        return true;
    }

    // Register this handler with the kernel interrupt system
    bool registerHandler() {
        if (is_registered_) return true;
        if (register_fn_) {
            register_fn_(vector, this);
        } else {
            oz::irq::registerHandler(vector, this);
        }
        is_registered_ = true;
        return true;
    }

    // Unregister this handler from the kernel interrupt system
    bool unregisterHandler() {
        if (!is_registered_) return false;
        if (unregister_fn_) {
            unregister_fn_(vector, this);
        } else {
            oz::irq::unregisterHandler(vector, this);
        }
        is_registered_ = false;
        return true;
    }
};

} // namespace oz
