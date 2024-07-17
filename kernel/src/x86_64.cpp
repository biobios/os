#include "x86_64.hpp"

#include <cstdint>
#include <cstddef>

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
        "movw $0, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n" ::
            : "%ax");

    std::uint16_t selector = 8;
    std::uint64_t dummy;
    __asm__ volatile(
        "movw $16, %%ax\n"
        "movw %%ax, %%ss\n"
        "pushq %[selector]\n"
        "movq $ret_label, %[dummy]\n"
        "pushq %[dummy]\n"
        "lretq\n"
        "ret_label:"
        : [dummy] "=r"(dummy)
        : [selector] "m"(selector)
        : "%ax");
}

namespace{
    struct InterruptDescriptor {
        std::uint16_t offset_00_15;
        std::uint16_t segment_selector;
        std::uint16_t flags;
        std::uint16_t offset_16_31;
        std::uint32_t offset_32_63;
        std::uint32_t reserved;
    };

    __attribute__((interrupt))
    void defaultHandler(void*){
        oz::x86_64::notifyEndOfInterrupt();
    }

    
    template<std::uint8_t index>
    __attribute__((interrupt))
    void Hoge(void*){
        oz::x86_64::notifyEndOfInterrupt();
    }

    InterruptDescriptor idt[oz::x86_64::MAX_INTERRUPTS_COUNT];

}

void oz::x86_64::setInterruptDescriptor(std::uint8_t index, void* handler){
    idt[index].offset_00_15 = reinterpret_cast<std::uint64_t>(handler) & 0xffff;
    idt[index].segment_selector = 8;
    idt[index].flags = 0x8e00;
    idt[index].offset_16_31 = (reinterpret_cast<std::uint64_t>(handler) >> 16) & 0xffff;
    idt[index].offset_32_63 = (reinterpret_cast<std::uint64_t>(handler) >> 32) & 0xffffffff;
    idt[index].reserved = 0;
}

void oz::x86_64::initIDTR() {
    for(std::size_t i = 0; i < MAX_INTERRUPTS_COUNT; i++){
        setInterruptDescriptor(i, reinterpret_cast<void*>(defaultHandler));
    }

    volatile std::uint64_t idtr[2];
    idtr[0] = (reinterpret_cast<std::uint64_t>(&idt) << 16) | (sizeof(idt) - 1);
    idtr[1] = (reinterpret_cast<std::uint64_t>(&idt) >> 48);
    __asm__ volatile("lidt %[idtr]" ::[idtr] "m"(idtr));
}

constexpr std::uint64_t END_OF_INTERRUPT_REGISTER_ADDR = 0xfee000b0;
__attribute__((no_caller_saved_registers))
void oz::x86_64::notifyEndOfInterrupt() {
    *reinterpret_cast<std::uint32_t*>(END_OF_INTERRUPT_REGISTER_ADDR) = 0;
}

void oz::x86_64::setPageMap(void* map) {
    volatile std::uint64_t pBuf = reinterpret_cast<std::uint64_t>(map);
    __asm__ volatile(
        "movq %0, %%cr3"
        ::"r"(pBuf)
    );
}

void oz::x86_64::enableSSE() {
    std::uint64_t dum[2];
    __asm__ volatile(
        "movq %%cr0, %%rax\n"
        "andq $-5, %%rax\n"//CR0.EM(bit2)をクリア
        "orq $2, %%rax\n"//CR0.MP(bit1)をセット
        "movq %%rax, %%cr0\n"
        "movq %%cr4, %%rax\n"
        "orq $1536, %%rax\n"//CR4.OSFXSR(bit9)をセット
        //CR4.OSXMMEXCPT(bit10)をセット
        "movq %%rax, %%cr4\n"
        : [dummy]"=m"(dum)
        :
        :"%rax", "%xmm0"
    );
}

std::uint8_t oz::x86_64::readIO8(std::uint16_t addr) {
    std::uint8_t value;
    __asm__ volatile(
        "inb %%dx, %%al\n"
        : "=a"(value) : "d"(addr)
    );
    return value;
}

std::uint16_t oz::x86_64::readIO16(std::uint16_t addr) {
    std::uint16_t value;
    __asm__ volatile(
        "inw %%dx, %%ax\n"
        : "=a"(value) : "d"(addr)
    );
    return value;
}

std::uint32_t oz::x86_64::readIO32(std::uint16_t addr) {
    std::uint32_t value;
    __asm__ volatile(
        "inl %%dx, %%eax\n"
        : "=a"(value) : "d"(addr)
    );
    return value;
}

void oz::x86_64::writeIO8(std::uint16_t addr, std::uint8_t value) {
    __asm__ volatile(
        "outb %[value], %[addr]"
        :: [value]"a"(value), [addr]"d"(addr)
    );
}

void oz::x86_64::writeIO16(std::uint16_t addr, std::uint16_t value) {
    __asm__ volatile(
        "outw %[value], %[addr]"
        :: [value]"a"(value), [addr]"d"(addr)
    );
}

void oz::x86_64::writeIO32(std::uint16_t addr, std::uint32_t value) {
    __asm__ volatile(
        "outl %[value], %[addr]"
        :: [value]"a"(value), [addr]"d"(addr)
    );
}

constexpr std::uint64_t localAPICIDPtr = 0xfee00020;

std::uint8_t oz::x86_64::getLocalAPICID() {
    return *reinterpret_cast<std::uint32_t*>(localAPICIDPtr) >> 24;
}
