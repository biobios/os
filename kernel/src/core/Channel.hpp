#pragma once

#include "core/Capability.hpp"
#include <oz/IPCProtocol.hpp>
#include <cstdint>

namespace oz {

// Internal message transferred between endpoints (kernel-side only)
// Uses lazy capability transfer: slot index is preserved and moved on rendezvous
struct InternalIPCMessage {
    IPCMessage user_msg{};
};

// Forward declaration
class Thread;
class Notification;

class ChannelEndpoint : public KernelObject {
private:
    ChannelEndpoint* peer{nullptr};
    ChannelMode mode_{ChannelMode::BLOCKING};

    // Waiter list for threads blocked on recv on THIS endpoint
    Thread* recv_waiter_head{nullptr};
    Thread* recv_waiter_tail{nullptr};

    // Waiter list for threads blocked on send on THIS endpoint
    Thread* send_waiter_head{nullptr};
    Thread* send_waiter_tail{nullptr};

    // Temporary handoff buffer when receiver was already waiting
    InternalIPCMessage rendezvous_msg{};
    bool has_rendezvous_msg{false};

    // For asynchronous interrupt notifications (IRQs)
    bool has_pending_notification{false};
    std::uint64_t pending_notification_data{0};

    // Bound notification for event multiplexing
    Notification* bound_notification_{nullptr};
    std::uint64_t bound_badge_{0};

    // Global registry intrusive list
    ChannelEndpoint* reg_prev_{nullptr};
    ChannelEndpoint* reg_next_{nullptr};

public:
    explicit ChannelEndpoint(bool untyped = false);
    
    ~ChannelEndpoint() override;

    friend void unbind_notification_from_all_channels(Notification* notif);

    void set_peer(ChannelEndpoint* p) {
        peer = p;
    }

    ChannelMode get_mode() const { return mode_; }
    void set_mode(ChannelMode m) { mode_ = m; }

    void on_peer_closed();

    bool is_peer_closed() const {
        return peer == nullptr;
    }

    ChannelEndpoint* get_peer() const {
        return peer;
    }

    // Recv waiter management
    void add_recv_waiter(Thread* thread);
    Thread* pop_recv_waiter();
    Thread* peek_recv_waiter() const { return recv_waiter_head; }
    bool remove_recv_waiter(Thread* thread);
    bool has_recv_waiters() const { return recv_waiter_head != nullptr; }

    // Send waiter management
    void add_send_waiter(Thread* thread);
    Thread* pop_send_waiter();
    Thread* peek_send_waiter() const { return send_waiter_head; }
    bool remove_send_waiter(Thread* thread);
    bool has_send_waiters() const { return send_waiter_head != nullptr; }

    // Rendezvous handoff message
    void set_rendezvous_msg(const InternalIPCMessage& msg) {
        rendezvous_msg = msg;
        has_rendezvous_msg = true;
    }

    bool pop_rendezvous_msg(InternalIPCMessage& out_msg) {
        if (!has_rendezvous_msg) return false;
        out_msg = rendezvous_msg;
        rendezvous_msg = InternalIPCMessage{};
        has_rendezvous_msg = false;
        return true;
    }

    bool has_rendezvous() const {
        return has_rendezvous_msg;
    }

    // Pending notifications (for IRQs)
    void set_notification(std::uint64_t data) {
        has_pending_notification = true;
        pending_notification_data = data;
    }

    bool check_and_clear_notification(std::uint64_t& data) {
        if (!has_pending_notification) return false;
        data = pending_notification_data;
        has_pending_notification = false;
        pending_notification_data = 0;
        return true;
    }

    bool has_notification() const {
        return has_pending_notification;
    }

    void bind_notification(Notification* notif, std::uint64_t badge);
    void unbind_notification();
    Notification* get_bound_notification() const { return bound_notification_; }
    std::uint64_t get_bound_badge() const { return bound_badge_; }
};

void unbind_notification_from_all_channels(Notification* notif);

} // namespace oz

