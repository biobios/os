#include "hardware/x86_64.hpp"

#include <cstdint>
#include <cstddef>

#include "memory/Address.hpp"

namespace {
    alignas(16) std::uint64_t gdt[8] = {
        0x0000000000000000, /* 0: NULL descriptor */
        0x00af9a000000ffff, /* 1: Kernel Code 64-bit */
        0x00cf93000000ffff, /* 2: Kernel Data */
        0x00cffa000000ffff, /* 3: User Code 32-bit (for sysret) */
        0x00cff2000000ffff, /* 4: User Data */
        0x00affa000000ffff, /* 5: User Code 64-bit */
        0x0000000000000000, /* 6: TSS Low */
        0x0000000000000000  /* 7: TSS High */
    };
    oz::x86_64::TaskStateSegment tss = {};
}

void oz::x86_64::initGDTR() {
    std::uint64_t tss_addr = reinterpret_cast<std::uint64_t>(&tss);
    std::uint64_t base_low = tss_addr & 0xffff;
    std::uint64_t base_mid = (tss_addr >> 16) & 0xff;
    std::uint64_t base_high = (tss_addr >> 24) & 0xff;
    std::uint64_t base_upper32 = tss_addr >> 32;

    gdt[6] = 0x0000890000000067 | (base_low << 16) | (base_mid << 32) | (base_high << 56);
    gdt[7] = base_upper32;
    tss.iomap_base = sizeof(tss);

    // GDTR requires 10 bytes: 2 bytes limit, 8 bytes base
    #pragma pack(push, 1)
    struct {
        std::uint16_t limit;
        std::uint64_t base;
    } gdtr;
    #pragma pack(pop)
    
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = reinterpret_cast<std::uint64_t>(gdt);

    __asm__ volatile("lgdt %[gdtr]" ::[gdtr] "m"(gdtr));

    __asm__ volatile(
        "movw %[ds], %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n" ::
            [ds] "i" (KERNEL_DS)
            : "%ax");

    std::uint64_t dummy;
    __asm__ volatile(
        "pushq %[selector]\n"
        "leaq 1f(%%rip), %[dummy]\n"
        "pushq %[dummy]\n"
        "lretq\n"
        "1:"
        : [dummy] "=r"(dummy)
        : [selector] "i"(KERNEL_CS));

    __asm__ volatile("ltr %%ax" :: "a"(TSS_SELECTOR));
}

void oz::x86_64::setKernelStack(std::uint64_t rsp) {
    tss.rsp0 = rsp;
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

constexpr std::uint64_t END_OF_INTERRUPT_REGISTER_ADDR = oz::DIRECT_MAP_OFFSET + 0xfee000b0ULL;
__attribute__((no_caller_saved_registers))
void oz::x86_64::notifyEndOfInterrupt() {
    *reinterpret_cast<volatile std::uint32_t*>(END_OF_INTERRUPT_REGISTER_ADDR) = 0;
}

void oz::x86_64::setPageMap(const void* map) {
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



std::uint8_t oz::x86_64::getLocalAPICID() {
    std::uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));
    return static_cast<std::uint8_t>(ebx >> 24);
}

void oz::x86_64::initAPICTimer(std::uint8_t interrupt_vector) {
    std::uintptr_t apic_base = oz::DIRECT_MAP_OFFSET + 0xfee00000ULL;
    volatile std::uint32_t* lvt_timer = reinterpret_cast<volatile std::uint32_t*>(apic_base + 0x320);
    volatile std::uint32_t* initial_count = reinterpret_cast<volatile std::uint32_t*>(apic_base + 0x380);
    volatile std::uint32_t* divide_config = reinterpret_cast<volatile std::uint32_t*>(apic_base + 0x3e0);

    // Set divide config to 16
    *divide_config = 0x3;

    // Set LVT Timer to Periodic Mode (bit 17) and set the interrupt vector
    *lvt_timer = (1 << 17) | interrupt_vector;

    // Set a default count
    *initial_count = 100000;
}
