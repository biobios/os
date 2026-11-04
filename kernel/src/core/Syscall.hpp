#pragma once
#include <cstdint>

namespace oz {

// ユーザースレッドとカーネルのスタックを切り替えるためのコンテキスト
struct SyscallContext {
    std::uint64_t user_rsp;
    std::uint64_t kernel_rsp;
};

void initSyscall();
void setSyscallKernelStack(std::uint64_t rsp);

extern SyscallContext g_syscall_context;

} // namespace oz
