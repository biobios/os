#include <cstdint>
#include <new>

#include "Kernel.hpp"
#include "utils.hpp"
#include "x86_64.hpp"

alignas(oz::Kernel) std::uint8_t k[sizeof(oz::Kernel)];
alignas(16) std::uint8_t stack[1024 * 1024 * 2];

extern "C" void main(oz_boot::PlatformInfo* platformInfo) {
    oz::x86_64::enableSSE();
    oz::x86_64::initGDTR();
    oz::x86_64::initIDTR();
    setKernelPtr(static_cast<void*>(&k));
    new (static_cast<void*>(&k)) oz::Kernel{platformInfo};
    reinterpret_cast<oz::Kernel*>(&k)->run();
    while (1) __asm__ volatile("hlt");
}

extern "C" void init(oz_boot::PlatformInfo* platformInfo) {
    std::uint8_t* stackBase = stack + sizeof(stack);

    __asm__ volatile(
        "movq %[boot_info], %%rdi\n"
        "movq %[stackPtr], %%rsp\n"
        "callq main\n" ::[boot_info] "r"(platformInfo),
        [stackPtr] "m"(stackBase)
        :);
}