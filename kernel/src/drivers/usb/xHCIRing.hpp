#pragma once

#include <cstdint>
#include <cstddef>
#include "drivers/usb/xHCI.hpp"

namespace xHCI {

class Ring {
public:
    Ring() : buf_(nullptr), ring_size_(0), cycle_bit_(1), enqueue_index_(0) {}
    
    void initialize(TRB::Any volatile* buf, std::size_t size);
    void push(const TRB::Any& trb);
    
    TRB::Any volatile* getBuffer() const { return buf_; }
    std::uint32_t getCycleBit() const { return cycle_bit_; }
    
private:
    TRB::Any volatile* buf_;
    std::size_t ring_size_;
    std::uint32_t cycle_bit_;
    std::size_t enqueue_index_;
};

class EventRing {
public:
    EventRing() : buf_(nullptr), ring_size_(0), cycle_bit_(1), dequeue_index_(0) {}
    
    void initialize(TRB::Any volatile* buf, std::size_t size);
    bool hasEvent();
    TRB::Any pop();
    std::size_t getDequeueIndex() const { return dequeue_index_; }
    TRB::Any volatile* getBuffer() const { return buf_; }

private:
    TRB::Any volatile* buf_;
    std::size_t ring_size_;
    std::uint32_t cycle_bit_;
    std::size_t dequeue_index_;
};

} // namespace xHCI
