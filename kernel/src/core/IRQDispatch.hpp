#pragma once
#include <cstdint>

namespace oz {
class IRQHandler;
class Thread;

namespace irq {
    void registerHandler(std::uint8_t vector, IRQHandler* handler);
    IRQHandler* getHandler(std::uint8_t vector);
    void setUnblockFn(void (*fn)(Thread*));
    void (*getUnblockFn())(Thread*);
}
}
