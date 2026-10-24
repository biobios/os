#pragma once
#include <cstdint>

namespace oz {

class ExecutionContext;
class InterruptThread;

struct SavedInterruptFrame {
    ExecutionContext* interrupted_context{nullptr};
    std::uint32_t lapic_remaining_ticks{0};
    std::uint8_t irq_vector{0};

    void clear() {
        interrupted_context = nullptr;
        lapic_remaining_ticks = 0;
        irq_vector = 0;
    }
};

extern InterruptThread* g_current_interrupt_thread;

} // namespace oz
