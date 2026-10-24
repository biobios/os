#pragma once
#include <cstdint>

namespace oz {
namespace x86_64 {

constexpr std::uint64_t MAX_INTERRUPTS_COUNT = 256;

constexpr std::uint32_t MSR_EFER = 0xC0000080;
constexpr std::uint32_t MSR_STAR = 0xC0000081;
constexpr std::uint32_t MSR_LSTAR = 0xC0000082;
constexpr std::uint32_t MSR_FMASK = 0xC0000084;
constexpr std::uint32_t MSR_FS_BASE = 0xC0000100;
constexpr std::uint32_t MSR_GS_BASE = 0xC0000101;
constexpr std::uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;

inline std::uint64_t readMSR(std::uint32_t msr) {
    std::uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

inline void writeMSR(std::uint32_t msr, std::uint64_t value) {
    std::uint32_t low = value & 0xFFFFFFFF;
    std::uint32_t high = value >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

inline std::uint64_t rdtsc() {
    std::uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

#pragma pack(push, 1)
struct TaskStateSegment {
    std::uint32_t reserved1;
    std::uint64_t rsp0;
    std::uint64_t rsp1;
    std::uint64_t rsp2;
    std::uint64_t reserved2;
    std::uint64_t ist[7];
    std::uint64_t reserved3;
    std::uint16_t reserved4;
    std::uint16_t iomap_base;
};
#pragma pack(pop)

struct ExceptionFrame {
    std::uint64_t rip;
    std::uint64_t cs;
    std::uint64_t rflags;
    std::uint64_t rsp;
    std::uint64_t ss;
};

constexpr std::uint16_t KERNEL_CS = 1 * 8;
constexpr std::uint16_t KERNEL_DS = 2 * 8;
constexpr std::uint16_t USER_CS32 = 3 * 8;
constexpr std::uint16_t USER_DS = 4 * 8;
constexpr std::uint16_t USER_CS = 5 * 8;
constexpr std::uint16_t TSS_SELECTOR = 6 * 8;
constexpr std::uint8_t APIC_TIMER_VECTOR = 32;

void initGDTR();
void setKernelStack(std::uint64_t rsp);
void setInterruptDescriptor(std::uint8_t index, void* handler);
void initIDTR();
void initIRQStubs();
__attribute__((no_caller_saved_registers))
void notifyEndOfInterrupt();
void setPageMap(const void* map);
void enableSSE();
std::uint8_t readIO8(std::uint16_t addr);
std::uint16_t readIO16(std::uint16_t addr);
std::uint32_t readIO32(std::uint16_t addr);
void writeIO8(std::uint16_t addr, std::uint8_t value);
void writeIO16(std::uint16_t addr, std::uint16_t value);
void writeIO32(std::uint16_t addr, std::uint32_t value);
std::uint8_t getLocalAPICID();
void initAPICTimer(std::uint8_t interrupt_vector);
void calibrateTimers();
void setLAPICOneShot(std::uint64_t time_budget_us, std::uint8_t vector = 32);
void setLAPICRemainingTicks(std::uint32_t ticks, std::uint8_t vector = 32);
void stopLAPICTimer();
std::uint32_t getLAPICCurrentCount();
void maskExternalInterrupts();
void unmaskExternalInterrupts();
bool isExternalInterruptsMasked();
std::uint64_t getTSCTicksPerUs();
std::uint64_t getLAPICTicksPerUs();
}
}  // namespace oz