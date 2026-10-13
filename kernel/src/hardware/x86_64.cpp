#include "hardware/x86_64.hpp"

#include <cstdint>
#include <cstddef>

#include "memory/Address.hpp"
#include "utils/utils.hpp"
#include "core/IRQDispatch.hpp"
#include "core/IRQHandler.hpp"
#include "core/Thread.hpp"
#include "utils/utils.hpp"

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
    void defaultHandler(oz::x86_64::ExceptionFrame* frame){
        oz::x86_64::notifyEndOfInterrupt();
    }

    void dumpExceptionInfo(const char* name, oz::x86_64::ExceptionFrame* frame, std::uint64_t error_code = 0) {
        char hexStr[17];
        dprint("\n\nEXCEPTION: ");
        dprint(name);
        dprint("\nRIP: 0x");
        oz::utils::to_hex(frame->rip, hexStr);
        dprint(hexStr);
        dprint("\nCS: 0x");
        oz::utils::to_hex(frame->cs, hexStr);
        dprint(hexStr);
        dprint("\nRFLAGS: 0x");
        oz::utils::to_hex(frame->rflags, hexStr);
        dprint(hexStr);
        dprint("\nRSP: 0x");
        oz::utils::to_hex(frame->rsp, hexStr);
        dprint(hexStr);
        dprint("\nSS: 0x");
        oz::utils::to_hex(frame->ss, hexStr);
        dprint(hexStr);
        dprint("\nError Code: 0x");
        oz::utils::to_hex(error_code, hexStr);
        dprint(hexStr);
        dprint("\nHALT.\n");
        while(true) { __asm__ volatile("hlt"); }
    }

    __attribute__((interrupt))
    void int00Handler(oz::x86_64::ExceptionFrame* frame) { dumpExceptionInfo("#DE Divide Error", frame); }

    __attribute__((interrupt))
    void int06Handler(oz::x86_64::ExceptionFrame* frame) { dumpExceptionInfo("#UD Invalid Opcode", frame); }

    __attribute__((interrupt))
    void int08Handler(oz::x86_64::ExceptionFrame* frame, std::uint64_t error_code) { dumpExceptionInfo("#DF Double Fault", frame, error_code); }

    __attribute__((interrupt))
    void int13Handler(oz::x86_64::ExceptionFrame* frame, std::uint64_t error_code) { dumpExceptionInfo("#GP General Protection", frame, error_code); }

    __attribute__((interrupt))
    void int14Handler(oz::x86_64::ExceptionFrame* frame, std::uint64_t error_code) { 
        std::uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        char hexStr[17];
        dprint("\nCR2: 0x");
        oz::utils::to_hex(cr2, hexStr);
        dprint(hexStr);
        dumpExceptionInfo("#PF Page Fault", frame, error_code); 
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

    setInterruptDescriptor(0, reinterpret_cast<void*>(int00Handler));
    setInterruptDescriptor(6, reinterpret_cast<void*>(int06Handler));
    setInterruptDescriptor(8, reinterpret_cast<void*>(int08Handler));
    setInterruptDescriptor(13, reinterpret_cast<void*>(int13Handler));
    setInterruptDescriptor(14, reinterpret_cast<void*>(int14Handler));

    volatile std::uint64_t idtr[2];
    idtr[0] = (reinterpret_cast<std::uint64_t>(&idt) << 16) | (sizeof(idt) - 1);
    idtr[1] = (reinterpret_cast<std::uint64_t>(&idt) >> 48);
    __asm__ volatile("lidt %[idtr]" ::[idtr] "m"(idtr));
}

namespace {
    __attribute__((no_caller_saved_registers))
    static void handleIRQ(std::uint8_t vector) {
        auto* handler = oz::irq::getHandler(vector);
        if (handler) {
            auto* waiter = handler->onInterrupt();
            if (waiter) {
                auto unblock = oz::irq::getUnblockFn();
                if (unblock) unblock(waiter);
            }
        }
        oz::x86_64::notifyEndOfInterrupt();
    }

#define IRQ_STUB(N) \
    __attribute__((interrupt)) \
    void irqStub##N(oz::x86_64::ExceptionFrame* frame) { \
        handleIRQ(0x##N); \
    }

    IRQ_STUB(30) IRQ_STUB(31) IRQ_STUB(32) IRQ_STUB(33)
    IRQ_STUB(34) IRQ_STUB(35) IRQ_STUB(36) IRQ_STUB(37)
    IRQ_STUB(38) IRQ_STUB(39) IRQ_STUB(3a) IRQ_STUB(3b)
    IRQ_STUB(3c) IRQ_STUB(3d) IRQ_STUB(3e) IRQ_STUB(3f)
    IRQ_STUB(40) IRQ_STUB(41) IRQ_STUB(42) IRQ_STUB(43)
    IRQ_STUB(44) IRQ_STUB(45) IRQ_STUB(46) IRQ_STUB(47)
    IRQ_STUB(48) IRQ_STUB(49) IRQ_STUB(4a) IRQ_STUB(4b)
    IRQ_STUB(4c) IRQ_STUB(4d) IRQ_STUB(4e) IRQ_STUB(4f)
}

void oz::x86_64::initIRQStubs() {
#define SET_STUB(N) setInterruptDescriptor(0x##N, reinterpret_cast<void*>(irqStub##N))
    SET_STUB(30); SET_STUB(31); SET_STUB(32); SET_STUB(33);
    SET_STUB(34); SET_STUB(35); SET_STUB(36); SET_STUB(37);
    SET_STUB(38); SET_STUB(39); SET_STUB(3a); SET_STUB(3b);
    SET_STUB(3c); SET_STUB(3d); SET_STUB(3e); SET_STUB(3f);
    SET_STUB(40); SET_STUB(41); SET_STUB(42); SET_STUB(43);
    SET_STUB(44); SET_STUB(45); SET_STUB(46); SET_STUB(47);
    SET_STUB(48); SET_STUB(49); SET_STUB(4a); SET_STUB(4b);
    SET_STUB(4c); SET_STUB(4d); SET_STUB(4e); SET_STUB(4f);
#undef SET_STUB
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
