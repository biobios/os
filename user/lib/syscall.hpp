#pragma once

#include <cstdint>
#include <cstddef>

namespace oz {

extern "C" {
    std::uint64_t syscall(std::uint64_t number, std::uint64_t arg1, std::uint64_t arg2, std::uint64_t arg3, std::uint64_t arg4, std::uint64_t arg5, std::uint64_t arg6);
    
    // Syscall wrappers
    void sys_exit(int status);
    std::size_t sys_write(const char* str);
}

} // namespace oz
