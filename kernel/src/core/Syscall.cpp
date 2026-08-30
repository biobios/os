#include "core/Syscall.hpp"
#include "hardware/x86_64.hpp"

#include "utils/utils.hpp"

namespace oz {

SyscallContext g_syscall_context;

extern "C" void syscall_entry();

void (*g_exit_thread_func)() = nullptr;

extern "C" std::uint64_t handleSyscall(std::uint64_t rdi, std::uint64_t rsi, std::uint64_t rdx,
                                       std::uint64_t r10, std::uint64_t r8, std::uint64_t r9,
                                       std::uint64_t syscall_number) {
    // 簡易的なシステムコールハンドラ（テスト用）
    if (syscall_number == 1) { // write
        dprint(reinterpret_cast<const char*>(rsi));
        return rdx; // bytes written
    } else if (syscall_number == 60) { // exit
        dprint("User process exited.\n\r");
        if (g_exit_thread_func) {
            g_exit_thread_func();
        }
        while(1) { __asm__ volatile("hlt"); }
    }
    return static_cast<std::uint64_t>(-1); // ENOSYS
}

void initSyscall() {
    std::uint64_t efer = oz::x86_64::readMSR(oz::x86_64::MSR_EFER);
    oz::x86_64::writeMSR(oz::x86_64::MSR_EFER, efer | 1);

    std::uint64_t star = (static_cast<std::uint64_t>(oz::x86_64::USER_CS32) << 48) | 
                         (static_cast<std::uint64_t>(oz::x86_64::KERNEL_CS) << 32);
    oz::x86_64::writeMSR(oz::x86_64::MSR_STAR, star);

    oz::x86_64::writeMSR(oz::x86_64::MSR_LSTAR, reinterpret_cast<std::uint64_t>(syscall_entry));
    oz::x86_64::writeMSR(oz::x86_64::MSR_FMASK, 0x200);
    oz::x86_64::writeMSR(oz::x86_64::MSR_KERNEL_GS_BASE, reinterpret_cast<std::uint64_t>(&g_syscall_context));
}

void setSyscallKernelStack(std::uint64_t rsp) {
    g_syscall_context.kernel_rsp = rsp;
}

__attribute__((naked)) void syscall_entry() {
    __asm__ volatile (
        "swapgs\n"
        "movq %rsp, %gs:0\n"
        "movq %gs:8, %rsp\n"

        "pushq %rcx\n"
        "pushq %r11\n"
        "pushq %r9\n"
        "pushq %r8\n"
        "pushq %r10\n"
        "pushq %rdx\n"
        "pushq %rsi\n"
        "pushq %rdi\n"
        
        "movq %rsp, %rbp\n"
        "andq $-16, %rsp\n"
        
        "movq %r10, %rcx\n"
        "movq %r8, %r8\n"
        "movq %r9, %r9\n"
        "pushq %rax\n"

        "call handleSyscall\n"

        "movq %rbp, %rsp\n"
        
        "popq %rdi\n"
        "popq %rsi\n"
        "popq %rdx\n"
        "popq %r10\n"
        "popq %r8\n"
        "popq %r9\n"
        "popq %r11\n"
        "popq %rcx\n"

        "movq %gs:0, %rsp\n"
        "swapgs\n"
        "sysretq\n"
    );
}

} // namespace oz
