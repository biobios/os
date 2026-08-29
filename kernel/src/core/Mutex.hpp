#pragma once
#include "utils/Spinlock.hpp"
#include "core/Thread.hpp"

namespace oz {

class Mutex {
private:
    Spinlock spinlock;
    bool locked = false;
    Thread* owner = nullptr;
    
    Thread* wait_queue_head = nullptr;
    Thread* wait_queue_tail = nullptr;

public:
    Mutex() = default;
    
    void lock();
    void unlock();
};

} // namespace oz
