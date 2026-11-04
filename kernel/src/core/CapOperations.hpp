#pragma once

#include "core/Capability.hpp"
#include "core/Channel.hpp"
#include "core/UntypedMemory.hpp"

namespace oz {

inline bool cap_move(CSpace* dest_cs, CapID dest, CSpace* src_cs, CapID src) {
    if (!dest_cs || !src_cs) return false;
    if (dest_cs == src_cs && dest == src) return true;

    Capability* src_slot = src_cs->getSlot(src);
    if (!src_slot || src_slot->is_null()) return false;

    Capability* dest_slot = dest_cs->getSlot(dest);
    if (!dest_slot || !dest_slot->is_null()) return false;

    // 異なる CSpace 間移動では TRANSFER 権限を検証
    if (dest_cs != src_cs) {
        if (!src_slot->has_rights(CapRights::TRANSFER)) {
            return false;
        }
    }

    return cdt_move_slot(dest_slot, src_slot);
}

inline bool cap_move(CSpace* cspace, CapID dest, CapID src) {
    return cap_move(cspace, dest, cspace, src);
}

inline bool cap_copy(CSpace* dest_cs, CapID dest, CSpace* src_cs, CapID src, CapRights requested_rights, std::uint64_t badge = 0, bool set_badge_flag = false) {
    if (!dest_cs || !src_cs) return false;

    Capability* src_slot = src_cs->getSlot(src);
    if (!src_slot || src_slot->is_null()) return false;

    // DUPLICATE 権限チェック (Linear は複製不可)
    if (!src_slot->can_duplicate()) return false;

    // PT裏付け中の Frame は複製不可
    if (src_slot->type() == CapType::Frame && src_slot->has_slot_flag(SLOT_FLAG_PT_BACKING)) {
        return false;
    }

    // 単一ビットマスクによる権限昇格防止チェック: (requested & ~parent) == 0
    if (requested_rights == CapRights::ALL) {
        requested_rights = src_slot->rights();
    } else if ((static_cast<std::uint32_t>(requested_rights) & ~static_cast<std::uint32_t>(src_slot->rights())) != 0) {
        return false;
    }

    Capability* dest_slot = dest_cs->getSlot(dest);
    if (!dest_slot || !dest_slot->is_null()) return false;

    // 生データコピー
    for (int i = 0; i < 4; ++i) dest_slot->data[i] = src_slot->data[i];
    dest_slot->flags = src_slot->flags;
    dest_slot->set_rights(requested_rights);

    // バッジ上書き (Mint 時)
    if (set_badge_flag) {
        dest_slot->data[1] = badge;
    }

    // KernelObject 参照型であれば add_ref
    if (dest_slot->has_slot_flag(SLOT_FLAG_HAS_KERNEL_OBJECT)) {
        if (KernelObject* obj = dest_slot->object()) {
            obj->add_ref();
        }
    }

    // Frame / PageTable / VSpace の複製時は未マップ状態に初期化
    if (dest_slot->type() == CapType::Frame ||
        dest_slot->type() == CapType::PageTable ||
        dest_slot->type() == CapType::VSpace) {
        dest_slot->clear_slot_flag(SLOT_FLAG_ORIGINAL | SLOT_FLAG_IS_MAPPED | SLOT_FLAG_PT_BACKING);
        dest_slot->set_map_entry_index(0);
        dest_slot->data[1] = 0;
        dest_slot->data[2] = 0;
        dest_slot->data[3] = 0;
    }

    // CDT 派生ツリーへ挿入
    cdt_insert_child(src_slot, dest_slot);
    return true;
}

inline bool cap_copy(CSpace* cspace, CapID dest, CapID src, CapRights rights) {
    return cap_copy(cspace, dest, cspace, src, rights, 0, false);
}

inline bool cap_delete(CSpace* cspace, CapID id) {
    if (!cspace) return false;
    return cspace->remove(id);
}

inline bool is_valid_channel_pair(ChannelMode m0, ChannelMode m1) {
    if (static_cast<std::uint8_t>(m0) > 2 || static_cast<std::uint8_t>(m1) > 2) return false;
    if (m0 == ChannelMode::RECV_NONBLOCK && m1 == ChannelMode::RECV_NONBLOCK) return false;
    if (m0 == ChannelMode::SEND_NONBLOCK && m1 == ChannelMode::SEND_NONBLOCK) return false;
    return true;
}

inline bool sys_channel_create(Capability* ut_slot,
                               CSpace* cs0, CapID slot0,
                               CSpace* cs1, CapID slot1,
                               ChannelMode mode0 = ChannelMode::BLOCKING,
                               ChannelMode mode1 = ChannelMode::BLOCKING) {
    if (!ut_slot || !cs0 || !cs1) return false;
    if (!is_valid_channel_pair(mode0, mode1)) return false;
    if (ut_slot->type() != CapType::UntypedMemory) return false;
    if (!ut_slot->has_rights(CapRights::DERIVE) && !ut_slot->has_rights(CapRights::WRITE)) return false;
    if (ut_slot->untyped_is_device()) return false;

    if (slot0 == 0 || slot0 >= CSpace::MAX_CAPS || slot1 == 0 || slot1 >= CSpace::MAX_CAPS) return false;
    if (is_reserved_system_slot(slot0) || is_reserved_system_slot(slot1)) return false;
    if (cs0 == cs1 && slot0 == slot1) return false;

    Capability* s0 = cs0->getSlot(slot0);
    Capability* s1 = cs1->getSlot(slot1);
    if (!s0 || !s0->is_null() || !s1 || !s1->is_null()) return false;

    std::size_t saved_watermark = ut_slot->untyped_watermark();
    ChannelEndpoint* end0 = untyped_allocate_object<ChannelEndpoint>(ut_slot, true);
    ChannelEndpoint* end1 = end0 ? untyped_allocate_object<ChannelEndpoint>(ut_slot, true) : nullptr;
    if (!end0 || !end1) {
        if (end0) end0->destroy();
        if (end1) end1->destroy();
        ut_slot->set_untyped_watermark(saved_watermark);
        return false;
    }

    end0->set_peer(end1);
    end1->set_peer(end0);
    end0->set_mode(mode0);
    end1->set_mode(mode1);

    init_kernel_object_slot(s0, end0, CapRights::ALL);
    init_kernel_object_slot(s1, end1, CapRights::ALL);

    cdt_insert_child(ut_slot, s0);
    cdt_insert_child(ut_slot, s1);

    return true;
}

} // namespace oz
