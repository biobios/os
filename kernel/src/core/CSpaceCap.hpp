#pragma once
#include "core/Capability.hpp"
#include "core/Thread.hpp"
#include <cstdint>

namespace oz {

// CSpaceCap: wraps a CSpace as a KernelObject, enabling cap-based management
// of other processes' capability spaces.
class CSpaceCap : public KernelObject {
private:
    CSpace* cspace_{nullptr};
    bool owned_{false};  // If true, CSpaceCap owns the CSpace and will delete it on destruction

public:
    CSpaceCap() : KernelObject(ObjectType::CSpaceCap, true) {}

    explicit CSpaceCap(CSpace* cs, bool owned = false, bool untyped = false)
        : KernelObject(ObjectType::CSpaceCap, untyped), cspace_(cs), owned_(owned) {}

    ~CSpaceCap() override {
        // 1. この CSpace を参照していた全 Thread の cspace ポインタを nullptr にクリア
        for_each_registered_thread([this](Thread* t) {
            if (t && t->cspace == this->cspace_) {
                t->cspace = nullptr;
            }
        });

        // 2. 内部の全有効スロットに対して cdt_remove_and_reparent を呼ぶ
        if (cspace_) {
            for (std::size_t i = 0; i < CSpace::MAX_CAPS; ++i) {
                Capability* slot = cspace_->getSlot(i);
                if (slot && !slot->is_null()) {
                    cdt_remove_and_reparent(slot);
                }
            }
            if (owned_ && !is_untyped_backed_) {
                delete cspace_;
            }
        }
    }

    CSpace* getCSpace() const { return cspace_; }
    void setCSpace(CSpace* cs, bool owned = false) {
        cspace_ = cs;
        owned_ = owned;
    }

    // Remove a capability from the wrapped CSpace
    bool removeCap(CapID slot) {
        if (!cspace_) return false;
        return cspace_->remove(slot);
    }
};

} // namespace oz
