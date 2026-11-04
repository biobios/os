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

void cdt_insert_child(Capability* parent, Capability* child) {
    if (!parent || !child) return;
    child->cdt_parent       = parent;
    child->cdt_next_child   = nullptr;
    child->cdt_next_sibling = parent->cdt_next_child;
    parent->cdt_next_child  = child;
}

bool cdt_move_slot(Capability* dest, Capability* src) {
    if (!dest || !src || dest == src || !dest->is_null() || src->is_null()) {
        return false;
    }

    for (int i = 0; i < 4; ++i) dest->data[i] = src->data[i];
    dest->flags            = src->flags;
    dest->cdt_parent       = src->cdt_parent;
    dest->cdt_next_child   = src->cdt_next_child;
    dest->cdt_next_sibling = src->cdt_next_sibling;

    if (Capability* parent = dest->cdt_parent) {
        if (parent->cdt_next_child == src) {
            parent->cdt_next_child = dest;
        } else {
            Capability* prev = parent->cdt_next_child;
            while (prev && prev->cdt_next_sibling != src) {
                prev = prev->cdt_next_sibling;
            }
            if (prev) {
                prev->cdt_next_sibling = dest;
            }
        }
    }

    for (Capability* c = dest->cdt_next_child; c != nullptr; c = c->cdt_next_sibling) {
        c->cdt_parent = dest;
    }

    relocate_pagetable_links_on_move(dest, src);

    src->clear_without_release();
    return true;
}

void cdt_remove_and_reparent(Capability* target) {
    if (!target || target->is_null()) return;

    if (target->has_slot_flag(SLOT_FLAG_IN_REVOKE)) {
        target->set_slot_flag(SLOT_FLAG_PENDING_DELETE);
        return;
    }

    if (target->type() == CapType::UntypedMemory && target->cdt_next_child != nullptr) {
        cdt_revoke_subtree(target);
        if (target->is_null()) return;
    }

    Capability* parent = target->cdt_parent;
    Capability* first_child = target->cdt_next_child;
    Capability* last_child = nullptr;
    for (Capability* c = first_child; c != nullptr; c = c->cdt_next_sibling) {
        c->cdt_parent = parent;
        last_child = c;
    }

    if (parent) {
        if (parent->cdt_next_child == target) {
            if (last_child) {
                last_child->cdt_next_sibling = target->cdt_next_sibling;
                parent->cdt_next_child = first_child;
            } else {
                parent->cdt_next_child = target->cdt_next_sibling;
            }
        } else {
            Capability* prev = parent->cdt_next_child;
            while (prev && prev->cdt_next_sibling != target) {
                prev = prev->cdt_next_sibling;
            }
            if (prev) {
                if (last_child) {
                    last_child->cdt_next_sibling = target->cdt_next_sibling;
                    prev->cdt_next_sibling = first_child;
                } else {
                    prev->cdt_next_sibling = target->cdt_next_sibling;
                }
            }
        }
    }

    target->cdt_parent       = nullptr;
    target->cdt_next_child   = nullptr;
    target->cdt_next_sibling = nullptr;
    target->release_and_clear();

    if (parent && parent->type() == CapType::UntypedMemory && parent->cdt_next_child == nullptr) {
        parent->set_untyped_watermark(0);
    }
}

void cdt_revoke_subtree(Capability* root) {
    if (!root || root->is_null()) return;

    root->set_slot_flag(SLOT_FLAG_IN_REVOKE);

    while (Capability* curr = root->cdt_next_child) {
        while (curr->cdt_next_child != nullptr) {
            curr = curr->cdt_next_child;
        }
        Capability* p = curr->cdt_parent;
        p->cdt_next_child = curr->cdt_next_sibling;

        curr->cdt_parent       = nullptr;
        curr->cdt_next_sibling = nullptr;
        if (curr->has_slot_flag(SLOT_FLAG_IN_REVOKE)) {
            curr->set_slot_flag(SLOT_FLAG_PENDING_DELETE);
        } else {
            curr->release_and_clear();
        }
    }

    if (root->type() == CapType::UntypedMemory) {
        root->set_untyped_watermark(0);
    }

    root->clear_slot_flag(SLOT_FLAG_IN_REVOKE);

    if (root->has_slot_flag(SLOT_FLAG_PENDING_DELETE)) {
        root->clear_slot_flag(SLOT_FLAG_PENDING_DELETE);
        cdt_remove_and_reparent(root);
    }
}

} // namespace oz
