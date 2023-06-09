#include "x86_64.hpp"

#include <cstdint>

namespace {
const std::uint64_t gdt[] = {
    0x0000000000000000, /* NULL descriptor */
    0x00af9a000000ffff, /* base=0, limit=4GB, mode=code(r-x),kernel */
    0x00cf93000000ffff  /* base=0, limit=4GB, mode=data(rw-),kernel */
};
}

void oz::x86_64::initGDTR() {
    volatile std::uint64_t gdtr[2];
    gdtr[0] = (reinterpret_cast<std::uint64_t>(&gdt) << 16) | (sizeof(gdt) - 1);
    gdtr[1] = (reinterpret_cast<std::uint64_t>(&gdt) >> 48);
    __asm__ volatile("lgdt %[gdtr]" ::[gdtr] "m"(gdtr));

    __asm__ volatile(
        "movw $2*8, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%ss\n" ::
            : "%ax");

    std::uint16_t selector = 8;
    std::uint64_t dummy;
    __asm__ volatile(
        "pushq %[selector]\n"
        "movq $ret_label, %[dummy]\n"
        "pushq %[dummy]\n"
        "lretq\n"
        "ret_label:"
        : [dummy] "=r"(dummy)
        : [selector] "m"(selector));
}

void oz::x86_64::setPageMap(void* map) {
    volatile std::uint64_t pBuf = reinterpret_cast<std::uint64_t>(map);
    __asm__ volatile(
        "movq %0, %%cr3"
        ::"r"(pBuf)
    );
}
