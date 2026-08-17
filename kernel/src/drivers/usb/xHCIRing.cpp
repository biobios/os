#include "drivers/usb/xHCIRing.hpp"

namespace xHCI {

void Ring::initialize(TRB::Any volatile* buf, std::size_t size) {
    buf_ = buf;
    ring_size_ = size;
    cycle_bit_ = 1;
    enqueue_index_ = 0;
}

void Ring::push(const TRB::Any& trb) {
    // dprint omitted inside here unless we declare extern dprint
    TRB::Any volatile* target = &buf_[enqueue_index_];
    target->data[0] = trb.data[0];
    target->data[1] = trb.data[1];
    target->data[2] = trb.data[2];
    
    std::uint32_t data3 = trb.data[3] & ~1; // cycle bitを一旦クリア
    data3 |= cycle_bit_;
    target->data[3] = data3;

    enqueue_index_++;
    if (enqueue_index_ == ring_size_ - 1) {
        // Link TRB
        TRB::Any volatile* link_trb = &buf_[enqueue_index_];
        link_trb->data[3] = (link_trb->data[3] & ~1) | cycle_bit_;
        // toggle cycle
        if ((link_trb->data[3] & 2) != 0) { // Toggle Cycle
            cycle_bit_ ^= 1;
        }
        enqueue_index_ = 0;
    }
}

void EventRing::initialize(TRB::Any volatile* buf, std::size_t size) {
    buf_ = buf;
    ring_size_ = size;
    cycle_bit_ = 1;
    dequeue_index_ = 0;
}

bool EventRing::hasEvent() {
    return (buf_[dequeue_index_].data[3] & 1) == cycle_bit_;
}

TRB::Any EventRing::pop() {
    TRB::Any event;
    event.data[0] = buf_[dequeue_index_].data[0];
    event.data[1] = buf_[dequeue_index_].data[1];
    event.data[2] = buf_[dequeue_index_].data[2];
    event.data[3] = buf_[dequeue_index_].data[3];
    
    dequeue_index_++;
    if (dequeue_index_ == ring_size_) {
        dequeue_index_ = 0;
        cycle_bit_ ^= 1;
    }
    return event;
}

} // namespace xHCI
