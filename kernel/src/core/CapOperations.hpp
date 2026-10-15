#pragma once

#include "core/Capability.hpp"
#include "core/Channel.hpp"
#include "core/UntypedMemory.hpp"

namespace oz {

inline bool cap_copy(CSpace* cspace, CapID dest, CapID src, CapRights rights) {
    (void)dest;
    Capability src_cap = cspace->get(src);
    if (!src_cap.is_valid()) return false;

    if ((src_cap.rights & rights) != rights) return false;

    Capability new_cap(src_cap.object, rights);
    cspace->add(new_cap); 
    return true;
}

inline bool cap_delete(CSpace* cspace, CapID id) {
    return cspace->remove(id);
}

// IPC send operation via a ChannelEndpoint capability
inline bool cap_channel_send(CSpace* cspace, CapID ep_cap_id, const IPCMessage& msg) {
    Capability ep_cap = cspace->get(ep_cap_id);
    if (!ep_cap.is_valid() || (ep_cap.rights & CapRights::WRITE) == CapRights::NONE) return false;

    ChannelEndpoint* ep = static_cast<ChannelEndpoint*>(ep_cap.object);
    return ep->send(msg);
}

// IPC receive operation via a ChannelEndpoint capability
inline bool cap_channel_recv(CSpace* cspace, CapID ep_cap_id, IPCMessage& msg) {
    Capability ep_cap = cspace->get(ep_cap_id);
    if (!ep_cap.is_valid() || (ep_cap.rights & CapRights::READ) == CapRights::NONE) return false;

    ChannelEndpoint* ep = static_cast<ChannelEndpoint*>(ep_cap.object);
    return ep->receive(msg);
}

// Syscall equivalent to create a channel and insert both ends into the CSpace
inline bool sys_channel_create(CSpace* cspace, CapID* out_id0, CapID* out_id1) {
    if (!cspace || !out_id0 || !out_id1) return false;

    Capability ut_cap = cspace->get(240);
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
