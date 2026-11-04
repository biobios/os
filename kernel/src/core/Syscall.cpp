#include "core/Syscall.hpp"
#include "hardware/x86_64.hpp"

namespace oz {

SyscallContext g_syscall_context;

extern "C" void syscall_entry();

void initSyscall() {
    std::uint64_t efer = oz::x86_64::readMSR(oz::x86_64::MSR_EFER);
    oz::x86_64::writeMSR(oz::x86_64::MSR_EFER, efer | (1ULL << 11) | 1);

    std::uint64_t star = (static_cast<std::uint64_t>(oz::x86_64::USER_CS32) << 48) | 
                         (static_cast<std::uint64_t>(oz::x86_64::KERNEL_CS) << 32);
    oz::x86_64::writeMSR(oz::x86_64::MSR_STAR, star);

    oz::x86_64::writeMSR(oz::x86_64::MSR_LSTAR, reinterpret_cast<std::uint64_t>(syscall_entry));
    oz::x86_64::writeMSR(oz::x86_64::MSR_FMASK, 0x200);
    oz::x86_64::writeMSR(oz::x86_64::MSR_KERNEL_GS_BASE, reinterpret_cast<std::uint64_t>(&g_syscall_context));
}

void setSyscallKernelStack(std::uint64_t rsp) {
    g_syscall_context.kernel_rsp = rsp;
    oz::x86_64::writeMSR(oz::x86_64::MSR_KERNEL_GS_BASE, reinterpret_cast<std::uint64_t>(&g_syscall_context));
}

__attribute__((naked)) void syscall_entry() {
    __asm__ volatile (
        "swapgs\n"
        "movq %rsp, %gs:0\n"
        "movq %gs:8, %rsp\n"
        "pushq %gs:0\n"        // Save user RSP on thread's own kernel stack
        
        "pushq %rbp\n"

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
        "pushq $0\n" 
        "pushq %rax\n"

        "call handleSyscall\n"

        // Update saved RDX slot with the 2nd return value from handleSyscall (System V ABI)
        "movq %rdx, 16(%rbp)\n"

        "movq %rbp, %rsp\n"
        
        "popq %rdi\n"
        "popq %rsi\n"
        "popq %rdx\n"
        "popq %r10\n"
        "popq %r8\n"
        "popq %r9\n"
        "popq %r11\n"
        "popq %rcx\n"
        
        "popq %rbp\n"
        "popq %rsp\n"          // Restore user RSP from kernel stack
        "swapgs\n"
        "sysretq\n"
    );
}

} // namespace oz
