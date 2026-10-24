#pragma once
#include "utils/Spinlock.hpp"
#include "core/Thread.hpp"
#include "utils/InterruptGuard.hpp"

namespace oz {

class Scheduler;

class Mutex {
private:
    Spinlock spinlock;
    bool locked = false;
    Thread* owner = nullptr;
    
    Thread* wait_queue_head = nullptr;
    Thread* wait_queue_tail = nullptr;

public:
    Mutex() = default;
    
    void lock(Scheduler& scheduler) {
        spinlock.lock();
        
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

    void unlock(Scheduler& scheduler) {
        spinlock.lock();
        
        if (wait_queue_head) {
            Thread* woke = wait_queue_head;
            wait_queue_head = woke->next_waiter;
            if (!wait_queue_head) {
                wait_queue_tail = nullptr;
            }
            
            owner = woke;
            woke->is_blocked = false;
            woke->state = ThreadState::Ready;
            if (woke->sched_notif && woke->sched_badge != 0) {
                woke->sched_notif->signal(woke->sched_badge);
            }
        } else {
            locked = false;
            owner = nullptr;
        }
        
        spinlock.unlock();
    }
};

} // namespace oz
