#pragma once

#include "core/Capability.hpp"
#include "core/Channel.hpp"
#include "core/UntypedMemory.hpp"

namespace oz {

inline bool cap_copy(CSpace* cspace, CapID dest, CapID src, CapRights rights) {
    Capability src_cap = cspace->get(src);
    if (!src_cap.is_valid()) return false;

    if ((src_cap.rights & rights) != rights) return false;

    Capability new_cap(src_cap.object, rights);
    return cspace->insert(dest, new_cap); 
}

inline bool cap_delete(CSpace* cspace, CapID id) {
    return cspace->remove(id);
}

// Syscall equivalent to create a channel and insert both ends into the CSpace
inline bool sys_channel_create(CSpace* cspace, CapID untyped_cap, CapID* out_id0, CapID* out_id1) {
    if (!cspace || !out_id0 || !out_id1) return false;

    Capability ut_cap = cspace->get(untyped_cap);
    if (!ut_cap.is_valid() || ut_cap.object->objectType() != ObjectType::UntypedMemory) return false;
    auto* untyped = static_cast<UntypedMemory*>(ut_cap.object);

    ChannelEndpoint* end0 = nullptr;
    ChannelEndpoint* end1 = nullptr;
    if (!untyped->retypeChannelPair(&end0, &end1)) return false;

    Capability cap0(end0, CapRights::ALL);
    Capability cap1(end1, CapRights::ALL);

    *out_id0 = cspace->add(cap0);
    *out_id1 = cspace->add(cap1);

    end0->release();
    end1->release();

    return true;
}

} // namespace oz
