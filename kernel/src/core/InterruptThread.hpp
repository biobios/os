#pragma once
#include "core/ExecutionContext.hpp"
#include "core/Capability.hpp"
#include "core/SavedInterruptFrame.hpp"

namespace oz {

class IRQHandler;

class InterruptThread : public KernelObject, public ExecutionContext {
public:
    std::uint8_t irq_vector{0};
    bool is_waiting_irq{false};                    // sys_irq_wait でハードウェア割り込み待機中か
    bool is_interrupted{false};                    // タイムアウトにより中断中か
    SavedInterruptFrame saved_frame{};             // 中断された下位コンテキストの退避領域
    InterruptThread* next_nested{nullptr};          // ネスト時のスタックリンク
    ExecutionContext* caller_context{nullptr};     // 再開時の親コンテキスト
    IRQHandler* irq_handler{nullptr};               // 関連づけられた IRQHandler
    ExecutionContext* replenisher_thread{nullptr}; // 即時パス用緊急対応スレッド（任意）

    InterruptThread(std::uint64_t id, void* stack, std::uint64_t size, PageBlock<> block, std::uint8_t level,
                    PhysicalAddress<PageTable> pml4_phys = PhysicalAddressProvider::nullPhysicalAddress<PageTable>(),
                    bool untyped = false);

    ~InterruptThread() override;

    bool isInterruptThread() const override { return true; }
};

} // namespace oz
