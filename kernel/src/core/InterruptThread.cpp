#include "core/InterruptThread.hpp"
#include "core/IRQHandler.hpp"

namespace oz {

InterruptThread* g_current_interrupt_thread = nullptr;

InterruptThread::InterruptThread(std::uint64_t id, void* stack, std::uint64_t size, PageBlock<> block, std::uint8_t level,
                                 PhysicalAddress<PageTable> pml4_phys, bool untyped)
    : KernelObject(ObjectType::InterruptThread, untyped),
      ExecutionContext(id, stack, size, block, level, pml4_phys) {
}

InterruptThread::~InterruptThread() {
    if (irq_handler && irq_handler->getHandlerThread() == this) {
        irq_handler->setHandlerThread(nullptr);
    }
    if (g_current_interrupt_thread == this) {
        g_current_interrupt_thread = next_nested;
    }
}

} // namespace oz
