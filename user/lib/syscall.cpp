#include "syscall.hpp"

namespace oz {

extern "C" {

std::uint64_t syscall(std::uint64_t number, std::uint64_t arg1, std::uint64_t arg2, std::uint64_t arg3, std::uint64_t arg4, std::uint64_t arg5, std::uint64_t arg6) {
    std::uint64_t ret;
    register std::uint64_t r10 __asm__("r10") = arg4;
    register std::uint64_t r8  __asm__("r8")  = arg5;
    register std::uint64_t r9  __asm__("r9")  = arg6;
    
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void sys_exit(int status) {
    syscall(60, static_cast<std::uint64_t>(status), 0, 0, 0, 0, 0);
    while (1) {
        __asm__ volatile("pause");
    }
}

std::size_t sys_write(const char* str) {
    // Currently the kernel syscall write uses rsi as the pointer
    return syscall(1, 0 /* fd */, reinterpret_cast<std::uint64_t>(str), 0, 0, 0, 0);
}

}

} // namespace oz
