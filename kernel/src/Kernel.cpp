#include "Kernel.hpp"

oz::Kernel::Kernel(PlatformInfo* platformInfo)
    : g(static_cast<Pixel*>(platformInfo->frame_buffer_base),
        platformInfo->frame_buffer_size, platformInfo->frame_buffer_horizontal,
        platformInfo->frame_buffer_vertical)
    , sh(&g){}

void oz::Kernel::run() {
    g.clearScreen();
    sh.printString("Finish init\n\rStart Kernel\n\r");
    while (1) {
        __asm__("hlt");
    }
}