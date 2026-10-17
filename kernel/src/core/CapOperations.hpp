#pragma once

#include "core/Capability.hpp"
#include "core/Channel.hpp"
#include "core/UntypedMemory.hpp"

namespace oz {

inline bool cap_move(CSpace* dest_cs, CapID dest, CSpace* src_cs, CapID src) {
    if (!dest_cs || !src_cs) return false;

    Capability src_cap = src_cs->get(src);
    if (!src_cap.is_valid()) return false;

    // Disallow overwriting an existing valid capability at destination
    Capability dest_cap = dest_cs->get(dest);
    if (dest_cap.is_valid() && !(dest_cs == src_cs && dest == src)) {
        return false;
    }

    if (dest_cs == src_cs && dest == src) {
        return true; // No-op move within same slot
    }

    if (!dest_cs->insert(dest, src_cap)) {
        return false;
    }

    src_cs->remove(src);
    return true;
}

inline bool cap_move(CSpace* cspace, CapID dest, CapID src) {
    return cap_move(cspace, dest, cspace, src);
}

inline bool cap_copy(CSpace* dest_cs, CapID dest, CSpace* src_cs, CapID src, CapRights rights) {
    if (!dest_cs || !src_cs) return false;

    Capability src_cap = src_cs->get(src);
    if (!src_cap.is_valid()) return false;

    // Disallow copying linear capabilities or capabilities lacking DUPLICATE right
    if (!src_cap.can_duplicate()) return false;

    // Disallow granting base rights not possessed by source
    CapRights base_requested = rights & ~CapRights::ONCE;
    if ((src_cap.rights & base_requested) != base_requested) return false;

    // Requesting ONCE attenuation requires WRITE right on source
    if ((rights & CapRights::ONCE) != CapRights::NONE) {
        if ((src_cap.rights & CapRights::WRITE) != CapRights::WRITE) return false;
    }

    Capability new_cap(src_cap.object, rights);
    return dest_cs->insert(dest, new_cap); 
}

inline bool cap_copy(CSpace* cspace, CapID dest, CapID src, CapRights rights) {
    return cap_copy(cspace, dest, cspace, src, rights);
}

inline bool cap_delete(CSpace* cspace, CapID id) {
    return cspace->remove(id);
}

// Create a channel endpoint pair with specified rights for each end
inline bool sys_channel_create_ext(CSpace* cspace, CapID untyped_cap, CapRights rights0, CapRights rights1, CapID* out_id0, CapID* out_id1) {
    if (!cspace || !out_id0 || !out_id1) return false;

    Capability ut_cap = cspace->get(untyped_cap);
    if (!ut_cap.is_valid() || ut_cap.object->objectType() != ObjectType::UntypedMemory) return false;
    auto* untyped = static_cast<UntypedMemory*>(ut_cap.object);

    ChannelEndpoint* end0 = nullptr;
    ChannelEndpoint* end1 = nullptr;
    if (!untyped->retypeChannelPair(&end0, &end1)) return false;

    Capability cap0(end0, rights0);
    Capability cap1(end1, rights1);

    *out_id0 = cspace->add(cap0);
    *out_id1 = cspace->add(cap1);

    end0->release();
    end1->release();

    if (*out_id0 == INVALID_CAP_ID || *out_id1 == INVALID_CAP_ID) {
        if (*out_id0 != INVALID_CAP_ID) cspace->remove(*out_id0);
        if (*out_id1 != INVALID_CAP_ID) cspace->remove(*out_id1);
        return false;
    }

    return true;
}

// Syscall equivalent to create a channel and insert both ends into the CSpace
inline bool sys_channel_create(CSpace* cspace, CapID untyped_cap, CapID* out_id0, CapID* out_id1) {
    return sys_channel_create_ext(cspace, untyped_cap, CapRights::ALL, CapRights::ALL, out_id0, out_id1);
}

} // namespace oz
