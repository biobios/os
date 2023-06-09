#include "Kernel.hpp"
#include "utils.hpp"
#include "x86_64.hpp"

namespace {
    oz::Kernel* kernelPtr;
}

oz::Kernel::Kernel(oz_boot::PlatformInfo* platformInfo)
    : g(static_cast<Pixel*>(platformInfo->frame_buffer_base),
        platformInfo->frame_buffer_size, platformInfo->frame_buffer_horizontal,
        platformInfo->frame_buffer_vertical)
    , sh(&g)
    , pm()
{
    kernelPtr = this;
    oz::x86_64::initGDTR();
    // char str[2*sizeof(std::uint64_t) + 1];
    // std::uint8_t* iter = reinterpret_cast<std::uint8_t*>(platformInfo->memory_map.buffer);
    // for(;iter < (reinterpret_cast<std::uint8_t*>(platformInfo->memory_map.buffer) + platformInfo->memory_map.map_size);
    //      iter += platformInfo->memory_map.descriptor_size){

    //         EFI_MEMORY_DESCRIPTOR* desc = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(iter);

    //         sh.printString("type = ");
    //         oz::utils::to_hex(desc->Type, str);
    //         sh.printString(str);
    //         sh.printString(", phys = ");
    //         oz::utils::to_hex(desc->PhysicalStart, str);
    //         sh.printString(str);
    //         sh.printString(" - ");
    //         oz::utils::to_hex(desc->PhysicalStart + desc->NumberOfPages * 4096 - 1, str);
    //         sh.printString(str);
    //         sh.printString(", pages = ");
    //         oz::utils::to_hex(desc->NumberOfPages, str);
    //         sh.printString(str);
    //         sh.printString(", attr = ");
    //         oz::utils::to_hex(desc->Attribute, str);
    //         sh.printString(str);
    //         sh.printString("\n\r");

    // }
}

void oz::Kernel::run() {
    g.clearScreen();
    sh.printString("Finish init\n\rStart Kernel\n\r");
    sh.repaint();
    while (1) {
        __asm__("hlt");
    }
}

void setKernelPtr(void* k) {
    kernelPtr = reinterpret_cast<oz::Kernel*>(k);
}

void dprint(const char* str) { kernelPtr->sh.printString(str); }
