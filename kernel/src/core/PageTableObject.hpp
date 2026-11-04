#pragma once

#include <cstdint>
#include <cstddef>
#include "core/Capability.hpp"
#include "memory/PageTable.hpp"
#include "memory/Address.hpp"

namespace oz {

class PageTableObject : public KernelObject, public PhysicalAddressProvider {
private:
    std::uintptr_t phys_addr_{0};                 // 8 bytes: バインドされた 4KB 物理ページアドレス (未バインド・無効化時は 0)
    Capability*    backing_frame_slot_{nullptr};  // 8 bytes: 裏付け 4KB ページを提供している Frame スロット
    Capability*    mapped_slots_head_{nullptr};   // 8 bytes: このテーブル内にマップされている子スロットのリスト
    Capability*    self_mapped_slots_head_{nullptr}; // 8 bytes: このテーブル自身を上位へマップしている PageTable スロットのリスト
    std::uint8_t   level_{1};                     // 1 byte: 1: PT, 2: PD, 3: PDPT, 4: PML4 (VSpace)
    std::uint8_t   reserved_[7]{0};               // 7 bytes: パディング (計56バイト)

public:
    explicit PageTableObject(std::uint8_t level = 1, bool untyped = true)
        : KernelObject(level == 4 ? ObjectType::VSpace : ObjectType::PageTable, untyped),
          level_(level) {}

    ~PageTableObject() override;

    std::uintptr_t phys_addr() const { return phys_addr_; }
    std::uint8_t level() const { return level_; }
    bool is_bound() const { return phys_addr_ != 0; }
    Capability* backing_frame_slot() const { return backing_frame_slot_; }
    void set_backing_frame_slot(Capability* slot) { backing_frame_slot_ = slot; }

    Capability* mapped_slots_head() const { return mapped_slots_head_; }
    Capability* self_mapped_slots_head() const { return self_mapped_slots_head_; }

    PageTable* hw_table() const {
        return phys_addr_ != 0 ? reinterpret_cast<PageTable*>(phys_addr_ + DIRECT_MAP_OFFSET) : nullptr;
    }

    // 裏付け 4KB Frame の排他性検証 (設計書 4.2節)
    static bool validate_backing_frame(const Capability* frame_slot) {
        if (!frame_slot || frame_slot->type() != CapType::Frame) return false;
        if (!frame_slot->has_rights(CapRights::READ | CapRights::WRITE)) return false;
        if (frame_slot->has_slot_flag(SLOT_FLAG_IS_MAPPED | SLOT_FLAG_PT_BACKING)) return false;
        if (!frame_slot->has_slot_flag(SLOT_FLAG_ORIGINAL)) return false;
        if (frame_slot->cdt_next_child != nullptr) return false;
        return true;
    }

    // バインド実行
    void bind_backing_frame(Capability* frame_slot, std::uint8_t level);

    // カーネルブートストラップ用直接バインド
    void bind_direct_phys(std::uintptr_t phys_addr, std::uint8_t level) {
        phys_addr_ = phys_addr;
        level_ = level;
        obj_type_ = (level == 4) ? ObjectType::VSpace : ObjectType::PageTable;
    }

    // 下位 PageTable のマップ
    int map_child_table(std::uint64_t idx, Capability* child_slot);

    // 末端 Frame のマップ
    int map_frame(std::uint64_t pt_idx, Capability* frame_slot, std::uint64_t req_flags);

    // 破棄・Revoke 処理
    void unmap_all_children();
    void unmap_self_from_parents();
    void evict_pml4_from_threads_and_cr3();
    void detach_backing_frame();

    // スロット移動時のリスト張り替えヘルパー
    void relocate_mapped_frame_slot(Capability* dest, Capability* src);
    void relocate_mapped_child_pt_slot(Capability* dest, Capability* src);
    void relocate_self_mapped_slot(Capability* dest, Capability* src);

    // VSpace 互換用アクセサ
    PhysicalAddress<PageTable> getPhysicalPML4() const { return createPhysicalAddress<PageTable>(phys_addr_); }

    friend bool unmap_capability_slot(Capability* slot);
};

static_assert(sizeof(PageTableObject) == 56, "PageTableObject must be 56 bytes");
static_assert(alignof(PageTableObject) == 8, "PageTableObject must be 8-byte aligned");

using VSpace = PageTableObject;

// TLB 一括フラッシュ機構
extern bool g_tlb_flush_pending;
inline void mark_tlb_flush_pending() { g_tlb_flush_pending = true; }
void flush_tlb_if_pending();

// Capability スロット指定 Unmap
bool unmap_capability_slot(Capability* slot);

} // namespace oz
