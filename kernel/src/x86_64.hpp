#pragma once
#include <cstdint>

namespace oz {
namespace x86_64 {

constexpr std::uint64_t MAX_INTERRUPTS_COUNT = 256;

void initGDTR();
void setInterruptDescriptor(std::uint8_t index, void* handler);
void initIDTR();
__attribute__((no_caller_saved_registers))
void notifyEndOfInterrupt();
void setPageMap(void* map);
void enableSSE();
std::uint8_t readIO8(std::uint16_t addr);
std::uint16_t readIO16(std::uint16_t addr);
std::uint32_t readIO32(std::uint16_t addr);
void writeIO8(std::uint16_t addr, std::uint8_t value);
void writeIO16(std::uint16_t addr, std::uint16_t value);
void writeIO32(std::uint16_t addr, std::uint32_t value);
std::uint8_t getLocalAPICID();
}
}  // namespace oz