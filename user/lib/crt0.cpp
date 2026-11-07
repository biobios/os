#include "syscall.hpp"
#include "cspace_allocator.hpp"
#include "../src/common/ipc/StartupInfo.hpp"

extern "C" void sys_exit(int status);

// Standard process entry point implemented by user processes
extern "C" void oz_main(const oz::ProcessStartupInfo* info);

extern "C" void _start_main(const oz::ProcessStartupInfo* info) {
    oz::g_slot_allocator.reset();
    if (info && info->magic == oz::ProcessStartupInfo::MAGIC) {
        for (std::size_t i = 0; i < info->num_caps; ++i) {
            oz::g_slot_allocator.reserve_slot(info->caps[i].slot);
        }
    }
    oz_main(info);
}

extern "C" __attribute__((weak, naked)) void _start() {
    __asm__ volatile (
        "movq (%%rsp), %%rdi\n"
        "andq $-16, %%rsp\n"
        "call _start_main\n"
        "movq $0, %%rdi\n"
        "call sys_exit\n"
        ::: "memory"
    );
}
