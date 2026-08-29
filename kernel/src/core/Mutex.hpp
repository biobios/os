#pragma once
#include "utils/Spinlock.hpp"
#include "core/Thread.hpp"
#include "utils/InterruptGuard.hpp"

namespace oz {

template <typename Accessor>
class Mutex {
private:
    Spinlock spinlock;
    bool locked = false;
    Thread* owner = nullptr;
    
    Thread* wait_queue_head = nullptr;
    Thread* wait_queue_tail = nullptr;

public:
    Mutex() = default;
    
    void lock() {
        spinlock.lock();
        auto& scheduler = Accessor::getScheduler();
        
        if (!locked) {
            locked = true;
            owner = scheduler.getCurrentThread();
            spinlock.unlock();
        } else {
            Thread* current = scheduler.getCurrentThread();
            current->state = ThreadState::Blocked;
            current->next_waiter = nullptr;
            
            if (wait_queue_tail) {
                wait_queue_tail->next_waiter = current;
                wait_queue_tail = current;
            } else {
                wait_queue_head = wait_queue_tail = current;
            }
            
            spinlock.unlock();
            
            scheduler.schedule();
        }
    }

    void unlock() {
        spinlock.lock();
        auto& scheduler = Accessor::getScheduler();
        
        if (wait_queue_head) {
            Thread* woke = wait_queue_head;
            wait_queue_head = woke->next_waiter;
            if (!wait_queue_head) {
                wait_queue_tail = nullptr;
            }
            
            owner = woke;
            scheduler.queueThread(woke);
        } else {
            locked = false;
            owner = nullptr;
        }
        
        spinlock.unlock();
    }
};

} // namespace oz
