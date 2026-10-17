#pragma once
#include <cstdint>
#include "memory/IFrameManager.hpp"
#include "memory/PageTable.hpp"
#include "memory/Address.hpp"
#include "core/Channel.hpp"

namespace oz {
class CSpace;

// Thread context saved on the stack during a context switch.
// Ordered from lowest address (top of stack) to highest address.
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wpadded"
struct ThreadContext {
    std::uint64_t r15;
    std::uint64_t r14;
    std::uint64_t r13;
    std::uint64_t r12;
    std::uint64_t rbp;
    std::uint64_t rbx;
    std::uint64_t rip; // Pushed by call instruction
};
#pragma GCC diagnostic pop

enum class ThreadState {
    Ready,
    Running,
    Blocked,
    BlockedOnRecv,
    BlockedOnSend,
    Dead
};

class Thread : public PhysicalAddressProvider {
public:
    std::uint64_t id;
    void* rsp;
    void* stack_base;
    std::uint64_t stack_size;
    PageBlock<> stack_block;   // The memory block for the stack
    std::uint8_t frame_level;
    
    ThreadState state;
    Thread* next;              // For Scheduler ready queue
    Thread* next_waiter;       // For Mutex wait queue / Channel wait queue
    Thread* next_zombie;       // For Reaper cleanup queue
    Thread* next_sleep;        // For Scheduler sleep queue
    std::uint64_t wake_up_tick; // Tick count when thread should wake up

    PhysicalAddress<PageTable> pml4_phys;
    void* user_data;
    void (*cleanup)(void*);
    
    std::uint64_t gs_base{0};
    std::uint64_t kernel_gs_base{0};

    // For saving user RSP during blocking syscalls
    std::uint64_t syscall_user_rsp{0};

    // For synchronous rendezvous IPC send
    InternalIPCMessage send_msg{};

    // For blocking IPC recv
    CapID recv_cap_id{0};
    IPCMessage* recv_msg_buf{nullptr};
    bool recv_result{false};

    bool is_untyped_backed{false};

    CSpace* cspace{nullptr};
    std::uint64_t user_rip{0};
    std::uint64_t user_rsp{0};

    Thread(std::uint64_t id, void* stack, std::uint64_t size, PageBlock<> block, std::uint8_t level, PhysicalAddress<PageTable> pml4_phys = PhysicalAddressProvider::nullPhysicalAddress<PageTable>())
        : id(id), rsp(reinterpret_cast<std::uint8_t*>(stack) + size), 
          stack_base(stack), stack_size(size), stack_block(block), frame_level(level),
          state(ThreadState::Ready), next(nullptr), next_waiter(nullptr), next_zombie(nullptr),
          pml4_phys(pml4_phys), user_data(nullptr), cleanup(nullptr) {}
};

extern "C" void switch_context(void** old_rsp, void* new_rsp);
extern "C" void thread_stub();

} // namespace oz
