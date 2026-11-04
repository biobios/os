#include "core/PageTableObject.hpp"
#include "core/Thread.hpp"
#include "core/Kernel.hpp"
#include "hardware/x86_64.hpp"
#include "core/ExecutionContext.hpp"

namespace oz {

bool g_tlb_flush_pending = false;

void flush_tlb_if_pending() {
    if (g_tlb_flush_pending) {
        x86_64::writeCR3(x86_64::readCR3());
        g_tlb_flush_pending = false;
    }
}

// 単方向リストからのアンリンク処理
static void unlink_singly_linked(Capability*& head, Capability* target, std::size_t next_idx) {
    if (head == target) {
        head = reinterpret_cast<Capability*>(target->data[next_idx]);
        return;
    }
    for (Capability* c = head; c; c = reinterpret_cast<Capability*>(c->data[next_idx])) {
        if (reinterpret_cast<Capability*>(c->data[next_idx]) == target) {
            c->data[next_idx] = target->data[next_idx];
            break;
        }
    }
}

// 単方向リスト内のノードポインタ付け替え処理
static void relocate_singly_linked(Capability*& head, Capability* dest, Capability* src, std::size_t next_idx) {
    if (head == src) {
        head = dest;
        return;
    }
    for (Capability* c = head; c; c = reinterpret_cast<Capability*>(c->data[next_idx])) {
        if (reinterpret_cast<Capability*>(c->data[next_idx]) == src) {
            c->data[next_idx] = reinterpret_cast<std::uint64_t>(dest);
            break;
        }
    }
}

PageTableObject::~PageTableObject() {
    unmap_all_children();
    unmap_self_from_parents();
    if (level_ == 4 && phys_addr_ != 0) {
        evict_pml4_from_threads_and_cr3();
    }
    if (backing_frame_slot_) {
        if (hw_table()) {
            hw_table()->clear();
        }
        backing_frame_slot_->clear_slot_flag(SLOT_FLAG_PT_BACKING);
        if (auto frame = TypedCap<FrameData>::from(backing_frame_slot_)) {
            frame->parent_pt = nullptr;
        }
        backing_frame_slot_ = nullptr;
    }
    phys_addr_ = 0;
}

void PageTableObject::detach_backing_frame() {
    unmap_all_children();
    unmap_self_from_parents();
    if (level_ == 4 && phys_addr_ != 0) {
        evict_pml4_from_threads_and_cr3();
    }
    if (hw_table()) {
        hw_table()->clear();
    }
    phys_addr_ = 0;
    backing_frame_slot_ = nullptr;
}

void PageTableObject::evict_pml4_from_threads_and_cr3() {
    for_each_registered_thread([this](Thread* t) {
        if (t && t->pml4_phys.get() == this->phys_addr_) {
            t->pml4_phys = PhysicalAddressProvider::nullPhysicalAddress<PageTable>();
        }
    });
    if (phys_addr_ != 0 && (x86_64::readCR3() & ~0xFFFULL) == (phys_addr_ & ~0xFFFULL)) {
        Kernel::instance().getKernelSpace().activate();
    }
}

void PageTableObject::bind_backing_frame(Capability* frame_slot, std::uint8_t level) {
    auto frame = TypedCap<FrameData>::from(frame_slot);
    if (!frame) return;

    phys_addr_ = frame->phys_addr;
    backing_frame_slot_ = frame_slot;
    level_ = level;
    obj_type_ = (level == 4) ? ObjectType::VSpace : ObjectType::PageTable;

    PageTable* hw = hw_table();
    if (hw) {
        hw->clear();
        if (level == 4) {
            PageTable* master = getMasterPML4();
            if (master) {
                for (std::size_t i = 256; i < 512; ++i) {
                    (*hw)[i] = (*master)[i];
                }
            }
        }
    }

    frame_slot->set_slot_flag(SLOT_FLAG_PT_BACKING);
    frame->parent_pt = this;
    frame->map_next  = nullptr;
    frame->map_prev  = nullptr;
}

int PageTableObject::map_child_table(std::uint64_t idx, Capability* child_slot) {
    if (!is_bound() || !child_slot) return static_cast<int>(SYS_ERR_INVALID);
    auto child = TypedCap<PageTableData>::from(child_slot);
    if (!child) return static_cast<int>(SYS_ERR_INVALID);

    auto* child_pt = child->self_pt;
    if (!child_pt || !child_pt->is_bound()) return static_cast<int>(SYS_ERR_INVALID);
    if (level_ != child_pt->level() + 1) return static_cast<int>(SYS_ERR_INVALID);
    if (level_ == 4 && idx >= 256) return static_cast<int>(SYS_ERR_INVALID);
    if (idx >= 512) return static_cast<int>(SYS_ERR_INVALID);
    if (child_slot->has_slot_flag(SLOT_FLAG_IS_MAPPED)) return static_cast<int>(SYS_ERR_INVALID);

    PageTable* hw = hw_table();
    if (!hw) return static_cast<int>(SYS_ERR_INVALID);
    if ((*hw)[idx].isPresent()) return static_cast<int>(SYS_ERR_BUSY);

    PageFlags pflags = PageFlags::Present | PageFlags::User;
    if (child_slot->has_rights(CapRights::WRITE)) {
        pflags = pflags | PageFlags::Writable;
    }
    if (!child_slot->has_rights(CapRights::EXEC)) {
        pflags = pflags | PageFlags::NoExecute;
    }

    (*hw)[idx].set(child_pt->phys_addr(), pflags);

    child_slot->set_slot_flag(SLOT_FLAG_IS_MAPPED);
    child_slot->set_map_entry_index(static_cast<std::uint16_t>(idx));
    child->parent_pt = this;

    // 親 mapped_slots_head_ (単方向)
    child->parent_map_next = mapped_slots_head_;
    mapped_slots_head_ = child_slot;

    // 子 self_mapped_slots_head_ (単方向)
    child->self_map_next = child_pt->self_mapped_slots_head_;
    child_pt->self_mapped_slots_head_ = child_slot;

    return 0;
}

int PageTableObject::map_frame(std::uint64_t pt_idx, Capability* frame_slot, std::uint64_t req_flags) {
    if (level_ != 1 || !is_bound() || pt_idx >= 512 || !frame_slot) return static_cast<int>(SYS_ERR_INVALID);
    auto frame = TypedCap<FrameData>::from(frame_slot);
    if (!frame) return static_cast<int>(SYS_ERR_INVALID);
    if (frame_slot->has_slot_flag(SLOT_FLAG_IS_MAPPED | SLOT_FLAG_PT_BACKING)) {
        return static_cast<int>(SYS_ERR_INVALID);
    }

    PageTable* hw = hw_table();
    if (!hw) return static_cast<int>(SYS_ERR_INVALID);
    if ((*hw)[pt_idx].isPresent()) return static_cast<int>(SYS_ERR_BUSY);

    PageFlags pflags = PageFlags::Present | PageFlags::User;
    if ((req_flags & 0x01) && frame_slot->has_rights(CapRights::WRITE)) {
        pflags = pflags | PageFlags::Writable;
    }
    if (!((req_flags & 0x02) && frame_slot->has_rights(CapRights::EXEC))) {
        pflags = pflags | PageFlags::NoExecute;
    }
    if ((req_flags & 0x10) || frame_slot->has_rights(CapRights::NOCACHE)) {
        pflags = pflags | PageFlags::CacheDisable;
    }

    (*hw)[pt_idx].set(frame->phys_addr, pflags);

    frame_slot->set_slot_flag(SLOT_FLAG_IS_MAPPED);
    frame_slot->set_map_entry_index(static_cast<std::uint16_t>(pt_idx));
    frame->parent_pt = this;

    // 双方向リスト
    frame->map_next = mapped_slots_head_;
    frame->map_prev = nullptr;
    if (mapped_slots_head_) {
        if (auto head_frame = TypedCap<FrameData>::from(mapped_slots_head_)) {
            head_frame->map_prev = frame_slot;
        }
    }
    mapped_slots_head_ = frame_slot;

    return 0;
}

bool unmap_capability_slot(Capability* slot) {
    if (!slot || !slot->has_slot_flag(SLOT_FLAG_IS_MAPPED)) return false;

    if (slot->type() == CapType::Frame) {
        auto frame = TypedCap<FrameData>::from(slot);
        if (!frame) return false;

        PageTableObject* parent_pt = frame->parent_pt;
        std::uint16_t idx = slot->map_entry_index();
        if (parent_pt && parent_pt->is_bound()) {
            if (PageTable* hw = parent_pt->hw_table()) {
                (*hw)[idx].clear();
                mark_tlb_flush_pending();
            }
        }
        Capability* next = frame->map_next;
        Capability* prev = frame->map_prev;
        if (prev) {
            if (auto prev_frame = TypedCap<FrameData>::from(prev)) {
                prev_frame->map_next = next;
            }
        } else if (parent_pt && parent_pt->mapped_slots_head_ == slot) {
            parent_pt->mapped_slots_head_ = next;
        }
        if (next) {
            if (auto next_frame = TypedCap<FrameData>::from(next)) {
                next_frame->map_prev = prev;
            }
        }
        slot->clear_slot_flag(SLOT_FLAG_IS_MAPPED);
        slot->set_map_entry_index(0);
        frame->parent_pt = nullptr;
        frame->map_next  = nullptr;
        frame->map_prev  = nullptr;
        return true;
    } else if (slot->type() == CapType::PageTable || slot->type() == CapType::VSpace) {
        auto pt = TypedCap<PageTableData>::from(slot);
        if (!pt) return false;

        PageTableObject* self_pt = pt->self_pt;
        PageTableObject* parent_pt = pt->parent_pt;
        std::uint16_t idx = slot->map_entry_index();
        if (parent_pt && parent_pt->is_bound()) {
            if (PageTable* hw = parent_pt->hw_table()) {
                (*hw)[idx].clear();
                mark_tlb_flush_pending();
            }
        }
        if (parent_pt) {
            unlink_singly_linked(parent_pt->mapped_slots_head_, slot, 2);
        }
        if (self_pt) {
            unlink_singly_linked(self_pt->self_mapped_slots_head_, slot, 3);
        }
        slot->clear_slot_flag(SLOT_FLAG_IS_MAPPED);
        slot->set_map_entry_index(0);
        pt->parent_pt       = nullptr;
        pt->parent_map_next = nullptr;
        pt->self_map_next   = nullptr;
        return true;
    }
    return false;
}

void PageTableObject::unmap_all_children() {
    while (mapped_slots_head_) {
        unmap_capability_slot(mapped_slots_head_);
    }
}

void PageTableObject::unmap_self_from_parents() {
    while (self_mapped_slots_head_) {
        unmap_capability_slot(self_mapped_slots_head_);
    }
}

void PageTableObject::relocate_mapped_frame_slot(Capability* dest, Capability* src) {
    auto frame = TypedCap<FrameData>::from(dest);
    if (!frame) return;

    if (frame->map_prev) {
        if (auto prev_frame = TypedCap<FrameData>::from(frame->map_prev)) {
            prev_frame->map_next = dest;
        }
    } else if (mapped_slots_head_ == src) {
        mapped_slots_head_ = dest;
    }
    if (frame->map_next) {
        if (auto next_frame = TypedCap<FrameData>::from(frame->map_next)) {
            next_frame->map_prev = dest;
        }
    }
}

void PageTableObject::relocate_mapped_child_pt_slot(Capability* dest, Capability* src) {
    relocate_singly_linked(mapped_slots_head_, dest, src, 2);
}

void PageTableObject::relocate_self_mapped_slot(Capability* dest, Capability* src) {
    relocate_singly_linked(self_mapped_slots_head_, dest, src, 3);
}

void relocate_pagetable_links_on_move(Capability* dest, Capability* src) {
    if (!dest || !src) return;
    if (dest->type() == CapType::Frame) {
        if (dest->has_slot_flag(SLOT_FLAG_PT_BACKING)) {
            if (auto frame = TypedCap<FrameData>::from(dest)) {
                if (frame->parent_pt) frame->parent_pt->set_backing_frame_slot(dest);
            }
        } else if (dest->has_slot_flag(SLOT_FLAG_IS_MAPPED)) {
            if (auto frame = TypedCap<FrameData>::from(dest)) {
                if (frame->parent_pt) frame->parent_pt->relocate_mapped_frame_slot(dest, src);
            }
        }
    } else if (dest->type() == CapType::PageTable || dest->type() == CapType::VSpace) {
        if (dest->has_slot_flag(SLOT_FLAG_IS_MAPPED)) {
            if (auto pt = TypedCap<PageTableData>::from(dest)) {
                if (pt->parent_pt) pt->parent_pt->relocate_mapped_child_pt_slot(dest, src);
                if (pt->self_pt) pt->self_pt->relocate_self_mapped_slot(dest, src);
            }
        }
    }
}

} // namespace oz
