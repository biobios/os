#include "core/Mutex.hpp"
#include "core/Scheduler.hpp"

namespace oz {

void Mutex::lock() {
    spinlock.lock();
    
    if (!locked) {
        locked = true;
        owner = getScheduler().getCurrentThread();
        spinlock.unlock();
    } else {
        Thread* current = getScheduler().getCurrentThread();
        current->state = ThreadState::Blocked;
        current->next_waiter = nullptr;
        
        if (wait_queue_tail) {
            wait_queue_tail->next_waiter = current;
            wait_queue_tail = current;
        } else {
            wait_queue_head = wait_queue_tail = current;
        }
        
        // We unlock before sleeping so other threads can manipulate the mutex.
        spinlock.unlock();
        
        // Sleep until we get the lock
        getScheduler().schedule();
    }
}

void Mutex::unlock() {
    spinlock.lock();
    
    if (wait_queue_head) {
        // Wake up the first waiting thread and pass ownership
        Thread* woke = wait_queue_head;
        wait_queue_head = woke->next_waiter;
        if (!wait_queue_head) {
            wait_queue_tail = nullptr;
        }
        
        owner = woke;
        getScheduler().queueThread(woke);
    } else {
        // No waiting threads, just unlock
        locked = false;
        owner = nullptr;
    }
    
    spinlock.unlock();
}

} // namespace oz
