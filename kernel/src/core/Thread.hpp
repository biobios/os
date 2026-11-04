#pragma once
#include <cstdint>
#include "core/ExecutionContext.hpp"
#include "core/Channel.hpp"
#include "core/SavedInterruptFrame.hpp"
#include "core/Baton.hpp"

namespace oz {

class ChannelEndpoint;
class Notification;
class IRQHandler;

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
    CapID recv_slot{0};
    IPCMessage* recv_msg_buf{nullptr};
    bool recv_result{false};
    int ipc_error{0};

    ChannelEndpoint* fault_ep{nullptr};

    // 親（スケジューラ）の起床通知用Notification
    Notification* sched_notif{nullptr};
    std::uint64_t sched_badge{0};

    // --- 割り込みハンドラ情報 ---
    IRQHandler* bound_irq_handler{nullptr};    // バインドされている IRQHandler (任意)
    Notification* bound_notification{nullptr}; // タイムアウト等を受け取る通知先
    SavedInterruptFrame saved_frame{};          // 割り込み時の被プリエンプト退避フレーム
    Thread* next_nested_irq{nullptr};           // ネスト割り込み時のスタックリンク

    // Baton Scheduling Context (Dual Baton Model)
    Baton* own_baton{nullptr};          // スレッド固有のバトン (sys_tcb_bind_baton で設定)
    Baton* borrowed_baton{nullptr};     // sys_call またはハードウェアIRQから借用したバトン

    Thread* reply_target{nullptr};      // 返信先スレッド (borrowed_baton の持主)
    Thread* pending_server{nullptr};    // 自身が sys_call 待機している相手サーバー

    // 現在ディスパッチされているアクティブバトン
    Baton* get_active_baton() const {
        return borrowed_baton ? borrowed_baton : own_baton;
    }

    inline bool is_irq_handling() const {
        return borrowed_baton && borrowed_baton->is_irq_baton;
    }

    // User-space scheduling and cancellation support
    ChannelEndpoint* blocking_ep{nullptr};
    bool ipc_cancelled{false};
    std::uint64_t current_reply_token{0};

    bool cancel_ipc();

    Thread(std::uint64_t id, void* stack, std::uint64_t size, PageBlock<> block, std::uint8_t level,
           PhysicalAddress<PageTable> pml4_phys = PhysicalAddressProvider::nullPhysicalAddress<PageTable>());
    ~Thread() override;

    bool isInterruptThread() const override { return false; }
};

void register_thread(Thread* t);
void unregister_thread(Thread* t);
Thread* get_thread_by_id(std::uint64_t id);
void dump_threads();

void for_each_registered_thread_impl(void (*callback)(Thread*, void*), void* data);

template <typename Fn>
inline void for_each_registered_thread(Fn&& fn) {
    auto cb = [](Thread* t, void* user) {
        (*reinterpret_cast<std::decay_t<Fn>*>(user))(t);
    };
    for_each_registered_thread_impl(cb, const_cast<void*>(reinterpret_cast<const void*>(&fn)));
}

extern "C" [[noreturn]] void load_context_noreturn(void* next_rsp);

extern "C" void switch_context(void** old_rsp, void* new_rsp);
extern "C" void thread_stub();

} // namespace oz
