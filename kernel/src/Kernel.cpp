#include "Kernel.hpp"
#include "utils.hpp"
#include "x86_64.hpp"
#include "oznew.hpp"

oz::Kernel::Kernel(oz_boot::PlatformInfo* platformInfo)
    : g(static_cast<Pixel*>(platformInfo->frame_buffer_base),
        platformInfo->frame_buffer_size, platformInfo->frame_buffer_horizontal,
        platformInfo->frame_buffer_vertical)
    , sh(&g)
    , pm()
    , fm(&platformInfo->memory_map, oz::paging::x86_64::page_sizes[0])
    , tlsf_malloc(&fm)
{
    setMemoryAllocator(&tlsf_malloc);
    // char str[2*sizeof(std::uint64_t) + 1];
    // std::uint8_t* iter = reinterpret_cast<std::uint8_t*>(platformInfo->memory_map.buffer);
    // std::uint64_t available_page = 0;
    // std::uint64_t not_available_page = 0;
    // for(;iter < (reinterpret_cast<std::uint8_t*>(platformInfo->memory_map.buffer) + platformInfo->memory_map.map_size);
    //      iter += platformInfo->memory_map.descriptor_size){

    //         oz_boot::EFI_MEMORY_DESCRIPTOR* desc = reinterpret_cast<oz_boot::EFI_MEMORY_DESCRIPTOR*>(iter);
    //         if(oz_boot::isAvailable(desc->Type)){
    //             // sh.printString(", phys = ");
    //             // oz::utils::to_hex(desc->PhysicalStart, str);
    //             // sh.printString(str);
    //             // sh.printString(" - ");
    //             // oz::utils::to_hex(desc->PhysicalStart + desc->NumberOfPages * 4096, str);
    //             // sh.printString(str);
    //             // sh.printString(", pages = ");
    //             // oz::utils::to_hex(desc->NumberOfPages, str);
    //             // sh.printString(str);
    //             // sh.printString("attr = ");
    //             // oz::utils::to_hex(desc->Attribute, str);
    //             // sh.printString(str);
    //             // sh.printString("\n\r");
    //             available_page += desc->NumberOfPages;
    //         }else{
    //             not_available_page += desc->NumberOfPages;
    //         }
    // }

    // sh.printString("avilable page count: ");
    // oz::utils::to_hex(available_page, str);
    // sh.printString(str);
    // sh.printString("\n\rnot avilable page count: ");
    // oz::utils::to_hex(not_available_page, str);
    // sh.printString(str);
    
}

void oz::Kernel::run() {
    g.clearScreen();
    sh.printString("Finish init\n\rStart Kernel\n\r");
    sh.repaint();
    while (1) {
        __asm__("hlt");
    }
}