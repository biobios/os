#include "utils/new_delete.hpp"
#include "utils/utils.hpp"
#include <cstddef>
#include <new>

// In a capability-based microkernel, dynamic heap allocation (unplaced operator new) is eliminated.
// All kernel objects must be retyped from UntypedMemory capabilities via placement new.
[[noreturn]] static void panic_no_heap() {
    dprint("\n\r[Kernel Panic] Kernel heap allocation (operator new) is disabled in capability-based OS!\n\r");
    while (true) {
        asm volatile("hlt");
    }
}

void* operator new(std::size_t) {
    panic_no_heap();
}
void* operator new[](std::size_t) {
    panic_no_heap();
}
void operator delete(void*) noexcept {}
void operator delete[](void*) noexcept {}
void operator delete(void*, std::size_t) noexcept {}
void operator delete[](void*, std::size_t) noexcept {}

