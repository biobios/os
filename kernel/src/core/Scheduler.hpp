#pragma once
#include "core/Thread.hpp"
#include "core/IScheduler.hpp"
#include "utils/InterruptGuard.hpp"
#include <cstdint>
#include <new>

namespace oz {

template <typename Accessor>
class Scheduler {
private:
    Thread* ready_queue_head = nullptr;
    Thread* ready_queue_tail = nullptr;
    Thread* current_thread = nullptr;
    
    Thread* zombie_queue_head = nullptr;
    Thread* zombie_queue_tail = nullptr;

    std::uint64_t next_thread_id = 1;

    static void exit_thread_stub(Scheduler* self) {
        self->exitThread();
    }

public:
    Scheduler() = default;

    Thread* createThread(std::uint8_t frame_level, void (*entry)(void*), void* arg) {
        auto& fm = Accessor::getFrameManager();
        auto& allocator = Accessor::getKernelMemoryAllocator();

        // 1. Allocate Stack via FrameManager
        PageBlock<> block = fm.allocateBlock(frame_level);
        void* stack = phys_to_virt(fm.getPhysicalAddress(block));
        std::uint64_t stack_size = (1ULL << frame_level) * fm.FRAME_SIZE;

        // 2. Allocate Thread object via KernelMemoryAllocator
        void* thread_obj_mem = allocator.malloc(sizeof(Thread));
        Thread* thread = new (thread_obj_mem) Thread(
            next_thread_id++, stack, stack_size, block, frame_level
        );

        // 3. Set up stack for switch_context
        std::uint64_t rsp = reinterpret_cast<std::uint64_t>(thread->stack_base) + thread->stack_size;
        rsp &= ~15ULL; // 16-byte align

        rsp -= sizeof(ThreadContext);
        thread->rsp = reinterpret_cast<void*>(rsp);

        ThreadContext* ctx = reinterpret_cast<ThreadContext*>(rsp);
        ctx->rip = reinterpret_cast<std::uint64_t>(&thread_stub);
        ctx->r12 = reinterpret_cast<std::uint64_t>(entry);
        ctx->rbx = reinterpret_cast<std::uint64_t>(arg);
        ctx->r13 = reinterpret_cast<std::uint64_t>(&Scheduler<Accessor>::exit_thread_stub);
        ctx->r14 = reinterpret_cast<std::uint64_t>(this); // Pass scheduler ptr
        ctx->r15 = 0;
        ctx->rbp = 0;

        return thread;
    }

    void initMainThread() {
        auto& allocator = Accessor::getKernelMemoryAllocator();
        void* thread_obj_mem = allocator.malloc(sizeof(Thread));
        Thread* thread = new (thread_obj_mem) Thread(
            next_thread_id++, nullptr, 0, PageBlock<>{nullptr}, 0
        );
        thread->state = ThreadState::Running;
        current_thread = thread;
    }

    void queueThread(Thread* thread) {
        InterruptGuard guard;

        thread->state = ThreadState::Ready;
        thread->next = nullptr;
        
        if (ready_queue_tail) {
            ready_queue_tail->next = thread;
            ready_queue_tail = thread;
        } else {
            ready_queue_head = ready_queue_tail = thread;
        }
    }

    void schedule() {
        InterruptGuard guard;

        if (!ready_queue_head) {
            return;
        }

        Thread* prev_thread = current_thread;
        Thread* next_thread = ready_queue_head;
        
        ready_queue_head = next_thread->next;
        if (!ready_queue_head) {
            ready_queue_tail = nullptr;
        }
        
        if (prev_thread && prev_thread->state == ThreadState::Running) {
            // Need to temporarily unlock to queue? No, queueThread uses its own guard, but wait...
            // If queueThread has a guard, and schedule has a guard, it will push/pop rflags again. 
            // pushfq/popq is safe to nest!
            queueThread(prev_thread);
        }
        
        next_thread->state = ThreadState::Running;
        current_thread = next_thread;
        
        if (prev_thread) {
            switch_context(&prev_thread->rsp, next_thread->rsp);
        } else {
            void* dummy_rsp;
            switch_context(&dummy_rsp, next_thread->rsp);
        }
    }

    void exitThread() {
        {
            InterruptGuard guard;
            current_thread->state = ThreadState::Dead;
            
            // Add to zombie queue
            current_thread->next_zombie = nullptr;
            if (zombie_queue_tail) {
                zombie_queue_tail->next_zombie = current_thread;
                zombie_queue_tail = current_thread;
            } else {
                zombie_queue_head = zombie_queue_tail = current_thread;
            }
        }
        
        schedule();
        
        while (1) __asm__ volatile("cli; hlt");
    }
    
    // Reaper / Idle thread entry
    static void idle_reaper_task(void* arg) {
        Scheduler* self = static_cast<Scheduler*>(arg);
        while (1) {
            self->reapZombies();
            __asm__ volatile("sti; hlt");
        }
    }

    void reapZombies() {
        Thread* current_zombie;
        {
            InterruptGuard guard;
            current_zombie = zombie_queue_head;
            zombie_queue_head = nullptr;
            zombie_queue_tail = nullptr;
        }
        
        auto& fm = Accessor::getFrameManager();
        auto& allocator = Accessor::getKernelMemoryAllocator();
        
        while (current_zombie) {
            Thread* next_z = current_zombie->next_zombie;
            
            // Free the stack frame
            if (current_zombie->stack_block.getDescriptor() != nullptr) {
                fm.freeBlock(current_zombie->stack_block);
            }
            
            // Free the Thread object
            allocator.free(current_zombie);
            
            current_zombie = next_z;
        }
    }

    Thread* getCurrentThread() { return current_thread; }
};

static_assert(scheduler<Scheduler<void>> || true); // Just a note

} // namespace oz
