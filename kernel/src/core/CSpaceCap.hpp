#pragma once
#include "core/Capability.hpp"
#include <cstdint>

namespace oz {

// CSpaceCap: wraps a CSpace as a KernelObject, enabling cap-based management
// of other processes' capability spaces.
class CSpaceCap : public KernelObject {
private:
    CSpace* cspace_;
    bool owned_;  // If true, CSpaceCap owns the CSpace and will delete it on destruction

public:
    explicit CSpaceCap(CSpace* cs, bool owned = false, bool untyped = false)
        : KernelObject(ObjectType::CSpaceCap, untyped), cspace_(cs), owned_(owned) {}

    ~CSpaceCap() override {
        if (owned_ && cspace_ && !is_untyped_backed_) {
            delete cspace_;
        }
    }

    CSpace* getCSpace() const { return cspace_; }

    // Insert a capability into the wrapped CSpace at a specific slot
    bool insertCap(CapID slot, const Capability& cap) {
        if (!cspace_) return false;
        return cspace_->insert(slot, cap);
    }

    // Remove a capability from the wrapped CSpace
    bool removeCap(CapID slot) {
        if (!cspace_) return false;
        return cspace_->remove(slot);
    }
};

} // namespace oz
