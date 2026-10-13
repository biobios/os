#pragma once

#include "core/Capability.hpp"
#include <cstdint>


namespace oz {

struct IPCMessage {
    std::uint64_t data[4];
    CapID transferred_cap;
    bool has_cap{false};
};

// Internal message stored in the channel queue (kernel-side only)
// Carries the actual Capability object when a cap is being transferred
struct InternalIPCMessage {
    IPCMessage user_msg;
    Capability attached_cap; // Valid only when user_msg.has_cap is true
};

// Forward declaration
class Thread;

class ChannelEndpoint : public KernelObject {
private:
    static constexpr std::size_t QUEUE_SIZE = 16;
    InternalIPCMessage msg_queue[QUEUE_SIZE];
    std::size_t q_head{0};
    std::size_t q_tail{0};
    std::size_t q_size{0};
    ChannelEndpoint* peer{nullptr};

    // Waiter list for threads blocked on recv on THIS endpoint
    Thread* waiter_head{nullptr};
    Thread* waiter_tail{nullptr};

public:
    ChannelEndpoint() : KernelObject(ObjectType::ChannelEndpoint) {}
    
    ~ChannelEndpoint() override {
        if (peer) {
            peer->on_peer_closed();
        }
    }

    void set_peer(ChannelEndpoint* p) {
        peer = p;
    }

    void on_peer_closed() {
        peer = nullptr;
    }

    bool is_peer_closed() const {
        return peer == nullptr;
    }

    // Send a message (with optional cap) to the peer endpoint
    bool send(const InternalIPCMessage& msg) {
        if (!peer) return false;
        peer->enqueue(msg);
        return true;
    }

    // Legacy send without cap (for backward compat)
    bool send(const IPCMessage& msg) {
        InternalIPCMessage internal;
        internal.user_msg = msg;
        // attached_cap is default-constructed (invalid)
        return send(internal);
    }

    // Pop a waiter from the PEER's wait list (called after send)
    // Returns a thread that was blocked on recv on the peer endpoint
    Thread* notify_peer_waiters() {
        if (!peer) return nullptr;
        return peer->pop_waiter();
    }

    void enqueue(const InternalIPCMessage& msg) {
        if (q_size >= QUEUE_SIZE) return;
        msg_queue[q_tail] = msg;
        q_tail = (q_tail + 1) % QUEUE_SIZE;
        q_size++;
    }

    bool receive(InternalIPCMessage& out_msg) {
        if (q_size == 0) {
            return false;
        }
        out_msg = msg_queue[q_head];
        msg_queue[q_head] = InternalIPCMessage{}; // Clear the slot (release any cap ref)
        q_head = (q_head + 1) % QUEUE_SIZE;
        q_size--;
        return true;
    }

    // Legacy receive (drops cap info)
    bool receive(IPCMessage& out_msg) {
        InternalIPCMessage internal;
        if (!receive(internal)) return false;
        out_msg = internal.user_msg;
        return true;
    }

    // Add a thread to THIS endpoint's waiter list
    void add_waiter(Thread* thread);

    // Remove and return the first waiting thread from THIS endpoint
    Thread* pop_waiter();

    bool has_waiters() const {
        return waiter_head != nullptr;
    }
};

inline void create_channel_pair(ChannelEndpoint** out_end0, ChannelEndpoint** out_end1) {
    *out_end0 = new ChannelEndpoint();
    *out_end1 = new ChannelEndpoint();
    (*out_end0)->set_peer(*out_end1);
    (*out_end1)->set_peer(*out_end0);
}

} // namespace oz

