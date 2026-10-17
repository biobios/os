#include "core/IRQDispatch.hpp"
#include "core/IRQHandler.hpp"

namespace oz {
namespace irq {

static IRQHandler* dispatch_table[256] = {};
static void (*unblock_fn)(Thread*) = nullptr;
static void (*tick_fn)() = nullptr;

void registerHandler(std::uint8_t vector, IRQHandler* handler) {
    dispatch_table[vector] = handler;
}

IRQHandler* getHandler(std::uint8_t vector) {
    return dispatch_table[vector];
}

void setUnblockFn(void (*fn)(Thread*)) {
    unblock_fn = fn;
}

void (*getUnblockFn())(Thread*) {
    return unblock_fn;
}

void setTimerTickFn(void (*fn)()) {
    tick_fn = fn;
}

void (*getTimerTickFn())() {
    return tick_fn;
}

}
}
