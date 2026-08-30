#include "core/Process.hpp"
#include "hardware/x86_64.hpp"

namespace oz {

__attribute__((naked)) void enter_user_mode(std::uint64_t rip, std::uint64_t user_rsp) {
    // rdi = rip, rsi = user_rsp
    __asm__ volatile (
        "cli\n"
        "movw %0, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        
        "pushq %0\n"        // SS = USER_DS | 3
        "pushq %%rsi\n"     // RSP = user_rsp
        "pushq $0x202\n"    // RFLAGS = IF=1
        "pushq %1\n"        // CS = USER_CS | 3
        "pushq %%rdi\n"     // RIP = rip
        
        "iretq\n"
        :: "i"(x86_64::USER_DS | 3), "i"(x86_64::USER_CS | 3)
    );
}

} // namespace oz
