#pragma once
#include "core/Capability.hpp"

namespace oz {

class IRQBatonControl : public KernelObject {
public:
    explicit IRQBatonControl(bool untyped = false)
        : KernelObject(ObjectType::IRQBatonControl, untyped) {}
    ~IRQBatonControl() override = default;
};

} // namespace oz
