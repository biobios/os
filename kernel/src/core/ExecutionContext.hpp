#pragma once
#include <cstdint>
#include "memory/IFrameManager.hpp"
#include "memory/PageTable.hpp"
#include "memory/Address.hpp"

namespace oz {

class CSpace;
class Baton;
class Notification;

// Thread context saved on the stack during a context switch.
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
    BlockedOnCall,
    BlockedOnReply,
    Dead
};

enum class SchedReason : std::uint64_t {
    NONE      = 0,
    TIMEOUT   = 1,
    YIELD     = 2,
    BLOCKED   = 3,
    EXIT      = 4,
    PREEMPTED = 5,
    FAULT     = 6,
    ORPHANED  = 7
};

constexpr SchedReason SYS_BATON_SWITCH_TIMEOUT = SchedReason::TIMEOUT;

constexpr std::int64_t SYS_ERR_INVALID      = -1;
constexpr std::int64_t SYS_ERR_WOULDBLOCK   = -2;
constexpr std::int64_t SYS_ERR_CANCELLED    = -3;
constexpr std::int64_t SYS_ERR_DEADLK       = -4;
constexpr std::int64_t SYS_ERR_INVALID_TYPE = -5;
constexpr std::int64_t SYS_ERR_BUSY         = -6;

struct SchedResult {
    SchedReason reason{SchedReason::NONE};
    std::uint64_t consumed_cycles{0};
};

class ExecutionContext : public PhysicalAddressProvider {
public:
    std::uint64_t id{0};
    void* rsp{nullptr};
    void* stack_base{nullptr};
    std::uint64_t stack_size{0};
    PageBlock<> stack_block{};
    std::uint8_t frame_level{0};

    ThreadState state{ThreadState::Ready};
    PhysicalAddress<PageTable> pml4_phys;
    void* user_data{nullptr};
    void (*cleanup)(void*){nullptr};

    std::uint64_t gs_base{0};
    std::uint64_t kernel_gs_base{0};
    std::uint64_t syscall_user_rsp{0};

    bool is_untyped_backed{false};
    CSpace* cspace{nullptr};
    std::uint64_t user_rip{0};
    std::uint64_t user_rsp{0};

    // Baton Scheduling Context (v2 Independent Capability Model)
    Baton* baton{nullptr};
    bool is_blocked{false};
    bool is_dead{false};
    std::uint64_t last_switched_in{0};
    std::uint64_t last_consumed_cycles{0};
    std::uint64_t total_cpu_cycles{0};
    std::uint64_t preempted_irq_vector{0};
    SchedReason sched_reason{SchedReason::NONE};

    Notification* exit_notif{nullptr};
    std::uint64_t exit_badge{0};

    ExecutionContext(std::uint64_t id, void* stack, std::uint64_t size, PageBlock<> block, std::uint8_t level,
                    PhysicalAddress<PageTable> pml4_phys = PhysicalAddressProvider::nullPhysicalAddress<PageTable>());
    virtual ~ExecutionContext();

    virtual bool isInterruptThread() const { return false; }

    bool has_baton() const;
    bool is_runnable() const {
        return state == ThreadState::Ready && has_baton();
    }

    void setSyscallReturnValue(SchedReason reason) {
        sched_reason = reason;
    }
};

} // namespace oz
