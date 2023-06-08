#include "Kernel.hpp"
#include <cstdint>
#include <new>

std::uint8_t k[sizeof(oz::Kernel)];

extern "C" void main(PlatformInfo* platformInfo) {
    new(static_cast<void*>(&k)) oz::Kernel{platformInfo};
    reinterpret_cast<oz::Kernel*>(&k)->run();
    while (1) __asm__ volatile("hlt");
}