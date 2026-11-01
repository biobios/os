#pragma once
#include <cstdint>

namespace oz {

class ExecutionContext;
class Thread;

struct SavedInterruptFrame {
    ExecutionContext* interrupted_context{nullptr}; // 被割り込みコンテキスト T (または下位ハンドラ)
    std::uint32_t lapic_remaining_ticks{0};         // T の LAPIC タイマー残りカウンタ
    std::uint32_t lapic_initial_ticks{0};           // ハンドラに設定した LAPIC 初期 Tick
    std::uint64_t entry_tsc{0};                     // 割り込み突入時の TSC
    std::uint8_t irq_vector{0};                     // 発生した IRQ ベクタ

    void clear() {
        interrupted_context = nullptr;
        lapic_remaining_ticks = 0;
        lapic_initial_ticks = 0;
        entry_tsc = 0;
        irq_vector = 0;
    }
};

extern Thread* g_current_irq_thread;

} // namespace oz
