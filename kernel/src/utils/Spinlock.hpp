#pragma once
#include <atomic>
#include <cstdint>

namespace oz {

class Spinlock {
private:
    std::atomic_flag flag = {};
    std::uint64_t saved_rflags = 0;

public:
    void lock() {
        std::uint64_t rflags;
        // Save RFLAGS and disable interrupts
        __asm__ volatile(
            "pushfq\n"
            "popq %0\n"
            "cli\n"
            : "=r"(rflags) :: "memory"
        );
        
        while (flag.test_and_set()) {
            __asm__ volatile("pause");
        }
        
        saved_rflags = rflags;
    }

    void unlock() {
        std::uint64_t rflags = saved_rflags;
        flag.clear();
        
        // Restore RFLAGS (this will re-enable interrupts if they were enabled before lock())
        __asm__ volatile(
            "pushq %0\n"
            "popfq\n"
            :: "r"(rflags) : "memory", "cc"
        );
    }
};

} // namespace oz
