#pragma once
#include <cstdint>

namespace oz {

class InterruptGuard {
private:
    std::uint64_t rflags;
public:
    InterruptGuard() {
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
    }
    ~InterruptGuard() {
        __asm__ volatile("pushq %0; popfq" :: "r"(rflags) : "memory", "cc");
    }

    // Prevent copying and moving
    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;
};

} // namespace oz
