#pragma once

#include "core/Capability.hpp"
#include "core/PageTableObject.hpp"
#include "core/VSpace.hpp"
#include "utils/utils.hpp"
#include <cstdint>

namespace oz {

inline int test_capability_lifecycle_algorithms() {
    // -------------------------------------------------------------------------
    // Test 1: Capability slot size & layout invariants
    // -------------------------------------------------------------------------
    static_assert(sizeof(Capability) == 64, "Capability must be exactly 64 bytes");
    static_assert(sizeof(PageTableObject) == 56, "PageTableObject must be exactly 56 bytes");
    static_assert(alignof(PageTableObject) == 8, "PageTableObject alignof must be 8");

    Capability root_slot;
    init_untyped_slot(&root_slot, 0x10000000, 0x200000, false);

    if (root_slot.type() != CapType::UntypedMemory) return 1;
    if (!root_slot.has_rights(CapRights::DERIVE | CapRights::REVOKE | CapRights::TRANSFER)) return 2;
    if (root_slot.can_duplicate()) return 3; // Untyped is linear, cannot duplicate
    if (root_slot.data[0] != 0x10000000 || root_slot.data[1] != 0x200000) return 4;

    // Bitfield test for slot flags and map entry index
    Capability test_cap;
    init_frame_slot(&test_cap, 0x20000000, CapRights::ALL, true);
    if (!test_cap.has_slot_flag(SLOT_FLAG_ORIGINAL)) return 5;
    test_cap.set_map_entry_index(384);
    if (test_cap.map_entry_index() != 384) return 6;
    test_cap.set_slot_flag(SLOT_FLAG_IS_MAPPED);
    test_cap.set_slot_flag(SLOT_FLAG_PT_BACKING);
    if (!test_cap.has_slot_flag(SLOT_FLAG_IS_MAPPED)) return 7;
    if (!test_cap.has_slot_flag(SLOT_FLAG_PT_BACKING)) return 8;
    if (test_cap.map_entry_index() != 384) return 9; // Index preserved

    // -------------------------------------------------------------------------
    // Test 2: Hierarchical CDT operations & Left-Child Right-Sibling reparenting
    // -------------------------------------------------------------------------
    Capability childA;
    Capability childB;
    Capability grandA1;
    Capability grandA2;

    init_frame_slot(&childA, 0x10000000, 4096);
    init_frame_slot(&childB, 0x10001000, 4096);
    init_frame_slot(&grandA1, 0x10000000, 4096);
    init_frame_slot(&grandA2, 0x10000000, 4096);

    // Build hierarchy:
    // root -> childB -> childA
    // childA -> grandA2 -> grandA1
    cdt_insert_child(&root_slot, &childA);
    cdt_insert_child(&root_slot, &childB);
    cdt_insert_child(&childA, &grandA1);
    cdt_insert_child(&childA, &grandA2);

    if (root_slot.cdt_next_child != &childB) return 10;
    if (childB.cdt_next_sibling != &childA) return 11;
    if (childA.cdt_parent != &root_slot) return 12;
    if (childA.cdt_next_child != &grandA2) return 13;
    if (grandA2.cdt_next_sibling != &grandA1) return 14;
    if (grandA1.cdt_parent != &childA) return 15;

    // Reparenting test: remove intermediate node childA
    // grandA1 and grandA2 should be reparented to root_slot
    cdt_remove_and_reparent(&childA);

    if (!childA.is_null()) return 16;
    if (childA.cdt_parent != nullptr || childA.cdt_next_child != nullptr || childA.cdt_next_sibling != nullptr) return 17;

    if (grandA1.cdt_parent != &root_slot) return 18;
    if (grandA2.cdt_parent != &root_slot) return 19;

    // Verify root children chain contains childB, grandA2, grandA1
    bool found_childB = false;
    bool found_grandA1 = false;
    bool found_grandA2 = false;
    for (Capability* c = root_slot.cdt_next_child; c != nullptr; c = c->cdt_next_sibling) {
        if (c == &childB) found_childB = true;
        if (c == &grandA1) found_grandA1 = true;
        if (c == &grandA2) found_grandA2 = true;
    }
    if (!found_childB || !found_grandA1 || !found_grandA2) return 20;

    // -------------------------------------------------------------------------
    // Test 3: Subtree Revoke
    // -------------------------------------------------------------------------
    cdt_revoke_subtree(&root_slot);

    if (root_slot.cdt_next_child != nullptr) return 21;
    if (!childB.is_null()) return 22;
    if (!grandA1.is_null()) return 23;
    if (!grandA2.is_null()) return 24;

    // -------------------------------------------------------------------------
    // Test 4: Untyped watermark auto-reset on last child delete
    // -------------------------------------------------------------------------
    root_slot.data[2] = 0x5000; // Simulated watermark
    Capability single_child;
    init_frame_slot(&single_child, 0x10000000, 4096);
    cdt_insert_child(&root_slot, &single_child);

    cdt_remove_and_reparent(&single_child);

    if (root_slot.data[2] != 0) return 30; // Watermark must be reset to 0
    if (root_slot.cdt_next_child != nullptr) return 31;

    // -------------------------------------------------------------------------
    // Test 5: Slot Move (cdt_move_slot)
    // -------------------------------------------------------------------------
    Capability move_src;
    Capability move_dest;
    init_frame_slot(&move_src, 0x20000000, 8192);
    cdt_insert_child(&root_slot, &move_src);

    if (!cdt_move_slot(&move_dest, &move_src)) return 40;
    if (!move_src.is_null()) return 41;
    if (move_dest.type() != CapType::Frame) return 42;
    if (move_dest.data[0] != 0x20000000 || move_dest.data[1] != 0) return 43;
    if (move_dest.cdt_parent != &root_slot) return 44;
    if (root_slot.cdt_next_child != &move_dest) return 45;

    cdt_remove_and_reparent(&move_dest);
    if (!move_dest.is_null()) return 46;

    // -------------------------------------------------------------------------
    // Test 6: cdt_move_slot with PageTable links (Pattern 1, 2, 3)
    // -------------------------------------------------------------------------
    std::uintptr_t dummy_phys = 0x10000000;

    // Pattern 1: SLOT_FLAG_PT_BACKING frame move
    {
        PageTableObject pt_obj(1);
        Capability bk_src;
        Capability bk_dest;
        init_frame_slot(&bk_src, dummy_phys, CapRights::ALL, true);
        pt_obj.bind_backing_frame(&bk_src, 1);
        if (!bk_src.has_slot_flag(SLOT_FLAG_PT_BACKING)) return 51;
        if (pt_obj.backing_frame_slot() != &bk_src) return 52;

        if (!cdt_move_slot(&bk_dest, &bk_src)) return 53;
        if (!bk_src.is_null()) return 54;
        if (pt_obj.backing_frame_slot() != &bk_dest) return 55;
        if (!bk_dest.has_slot_flag(SLOT_FLAG_PT_BACKING)) return 56;
    }

    // Pattern 2: Doubly-linked list of IS_MAPPED frames in Level 1 PT
    {
        std::uintptr_t pt_page_phys = 0x10001000;
        PageTableObject pt_l1(1);
        Capability bk_pt;
        init_frame_slot(&bk_pt, pt_page_phys, CapRights::ALL, true);
        pt_l1.bind_backing_frame(&bk_pt, 1);

        Capability f1, f2, f3;
        Capability f1_dest, f2_dest, f3_dest;
        init_frame_slot(&f1, 0x10004000, CapRights::ALL, true);
        init_frame_slot(&f2, 0x10005000, CapRights::ALL, true);
        init_frame_slot(&f3, 0x10006000, CapRights::ALL, true);

        if (pt_l1.map_frame(0, &f1, 0x01) != 0) return 61;
        if (pt_l1.map_frame(1, &f2, 0x01) != 0) return 62;
        if (pt_l1.map_frame(2, &f3, 0x01) != 0) return 63;

        // Move middle slot f2 -> f2_dest
        if (!cdt_move_slot(&f2_dest, &f2)) return 64;
        // Move head slot f3 -> f3_dest (head because map_frame inserts at head)
        if (!cdt_move_slot(&f3_dest, &f3)) return 65;
        // Move tail slot f1 -> f1_dest
        if (!cdt_move_slot(&f1_dest, &f1)) return 66;

        // Verify all 3 slots are still in the mapped list
        int count = 0;
        for (Capability* cur = pt_l1.mapped_slots_head(); cur != nullptr; cur = reinterpret_cast<Capability*>(cur->data[2])) {
            count++;
        }
        if (count != 3) return 67;

        // Unmap all children
        pt_l1.unmap_all_children();
        if (f1_dest.has_slot_flag(SLOT_FLAG_IS_MAPPED)) return 68;
        if (f2_dest.has_slot_flag(SLOT_FLAG_IS_MAPPED)) return 69;
        if (f3_dest.has_slot_flag(SLOT_FLAG_IS_MAPPED)) return 70;
        if (pt_l1.mapped_slots_head() != nullptr) return 71;
    }

    // Pattern 3: Shared PageTable slot move
    {
        std::uintptr_t pd_page_phys = 0x10002000;
        std::uintptr_t pt_page2_phys = 0x10003000;
        PageTableObject pd(2);
        PageTableObject pt(1);
        Capability bk_pd, bk_pt2;
        init_frame_slot(&bk_pd, pd_page_phys, CapRights::ALL, true);
        init_frame_slot(&bk_pt2, pt_page2_phys, CapRights::ALL, true);
        pd.bind_backing_frame(&bk_pd, 2);
        pt.bind_backing_frame(&bk_pt2, 1);

        Capability pt_slot, pt_slot_dest;
        init_pagetable_object_slot(&pt_slot, &pt, 1, CapRights::ALL);

        if (pd.map_child_table(10, &pt_slot) != 0) return 82;
        if (pd.mapped_slots_head() != &pt_slot) return 83;
        if (pt.self_mapped_slots_head() != &pt_slot) return 84;

        if (!cdt_move_slot(&pt_slot_dest, &pt_slot)) return 85;
        if (pd.mapped_slots_head() != &pt_slot_dest) return 86;
        if (pt.self_mapped_slots_head() != &pt_slot_dest) return 87;

        pt.unmap_self_from_parents();
        if (pd.mapped_slots_head() != nullptr) return 88;
        if (pt.self_mapped_slots_head() != nullptr) return 89;
    }

    return 0; // All algorithms passed
}

} // namespace oz
