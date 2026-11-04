#include "core/Capability.hpp"
#include "core/PageTableObject.hpp"
#include "core/IRQHandler.hpp"

namespace oz {

void Capability::release_and_clear() {
    if (is_null()) return;

    if (has_slot_flag(SLOT_FLAG_IS_MAPPED)) {
        unmap_capability_slot(this);
    } else if (type() == CapType::Frame && has_slot_flag(SLOT_FLAG_PT_BACKING)) {
        if (auto frame = TypedCap<FrameData>::from(this)) {
            if (frame->parent_pt) {
                frame->parent_pt->detach_backing_frame();
            }
        }
    }

    if (type() == CapType::IRQHandler) {
        if (IRQHandler* handler = irq_handler_entry<IRQHandler>()) {
            handler->unregisterAndReset();
        } else {
            g_irq_handlers[irq_vector()].unregisterAndReset();
        }
    }

    if (has_slot_flag(SLOT_FLAG_HAS_KERNEL_OBJECT)) {
        if (KernelObject* obj = object()) {
            obj->release();
        }
    }

    clear_without_release();
}

} // namespace oz
