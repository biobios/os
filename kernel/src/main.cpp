#include "Kernel.hpp"

extern "C" void main(PlatformInfo* platformInfo) {
    oz::Kernel kernel{platformInfo};
    kernel.run();
    while (1) __asm__ volatile("hlt");
}