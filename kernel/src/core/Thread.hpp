#pragma once
#include <cstdint>
#include "core/ExecutionContext.hpp"
#include "core/Channel.hpp"

namespace oz {

class ChannelEndpoint;
class Notification;

class Thread : public ExecutionContext {
public:
    Thread* next{nullptr};              // For Scheduler ready queue
    Thread* next_waiter{nullptr};       // For Mutex wait queue / Channel wait queue
    Thread* next_zombie{nullptr};       // For Reaper cleanup queue
    Thread* next_sleep{nullptr};        // For Scheduler sleep queue
    std::uint64_t wake_up_tick{0};      // Tick count when thread should wake up

    // For synchronous rendezvous IPC send
    InternalIPCMessage send_msg{};
    IPCMessage kernel_ipc_msg{};
    bool has_kernel_ipc_msg{false};

    // For blocking IPC recv
    CapID recv_cap_id{0};
    IPCMessage* recv_msg_buf{nullptr};
    bool recv_result{false};

    ChannelEndpoint* fault_ep{nullptr};

    // 親（スケジューラ）の起床通知用Notification
    Notification* sched_notif{nullptr};
    std::uint64_t sched_badge{0};

    // User-space scheduling and cancellation support
    ChannelEndpoint* blocking_ep{nullptr};
    bool ipc_cancelled{false};
    std::uint64_t current_reply_token{0};

    bool is_irq_handler{false}; // 後方互換用（InterruptThread 移行）

    bool cancel_ipc();

    Thread(std::uint64_t id, void* stack, std::uint64_t size, PageBlock<> block, std::uint8_t level,
           PhysicalAddress<PageTable> pml4_phys = PhysicalAddressProvider::nullPhysicalAddress<PageTable>());
    ~Thread() override;

    bool isInterruptThread() const override { return is_irq_handler; }
};

void register_thread(Thread* t);
void unregister_thread(Thread* t);
Thread* get_thread_by_id(std::uint64_t id);
void dump_threads();

extern "C" void switch_context(void** old_rsp, void* new_rsp);
extern "C" void thread_stub();

} // namespace oz
