#include "core/IRQDispatch.hpp"
#include "core/IRQHandler.hpp"

namespace oz {

IRQHandler g_irq_handlers[256];

namespace irq {

static IRQHandler* dispatch_table[256] = {};
static void (*unblock_fn)(Thread*) = nullptr;
static void (*tick_fn)() = nullptr;

void registerHandler(std::uint8_t vector, IRQHandler* handler) {
    if (dispatch_table[vector] == nullptr) {
        dispatch_table[vector] = handler;
    }
}

bool unregisterHandler(std::uint8_t vector, IRQHandler* handler) {
    if (dispatch_table[vector] == handler) {
        dispatch_table[vector] = nullptr;
        return true;
    }
    return false;
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
