#pragma once

#include <cstddef>
#include <cstdint>

namespace oz {
namespace utils {

template <typename T, std::size_t Capacity>
class RingBuffer {
public:
    RingBuffer() : read_index_(0), write_index_(0), count_(0) {}

    bool push(const T& item) {
        if (count_ >= Capacity) {
            return false; // Buffer full
        }
        buffer_[write_index_] = item;
        write_index_ = (write_index_ + 1) % Capacity;
        count_++;
        return true;
    }

    bool pop(T& out_item) {
        if (count_ == 0) {
            return false; // Buffer empty
        }
        out_item = buffer_[read_index_];
        read_index_ = (read_index_ + 1) % Capacity;
        count_--;
        return true;
    }

    bool isEmpty() const {
        return count_ == 0;
    }

    bool isFull() const {
        return count_ == Capacity;
    }

    std::size_t size() const {
        return count_;
    }

private:
    T buffer_[Capacity];
    std::size_t read_index_;
    std::size_t write_index_;
    std::size_t count_;
};

} // namespace utils
} // namespace oz
