#include "Kernel.hpp"
#include "Address.hpp"
#include "oznew.hpp"
#include "utils.hpp"
#include "x86_64.hpp"

oz::Kernel::Kernel(oz_boot::PlatformInfo* platformInfo)
    : g(static_cast<Pixel*>(platformInfo->frame_buffer_base),
        platformInfo->frame_buffer_size, platformInfo->frame_buffer_horizontal,
        platformInfo->frame_buffer_vertical)
    , sh(&g)
    , fm(&platformInfo->memory_map, oz::paging::x86_64::page_sizes[0])
    , tlsf_malloc(&fm)
    , pt_manager(getMasterPML4(), &fm)
    , kernel_space(getMasterPML4(), createPhysicalAddress<std::uint64_t>(reinterpret_cast<std::uintptr_t>(getMasterPML4()) - oz::KERNEL_VIRT_OFFSET), &fm)
{
    setMemoryAllocator(&tlsf_malloc);
}

void oz::Kernel::run() {
    g.clearScreen();
    sh.printString("Finish init\n\rStart Kernel in Higher-Half!\n\r");
    sh.repaint();
    while (1) {
        __asm__("hlt");
    }
}