#pragma once
#include <cstdint>

namespace oz {
namespace x86_64 {
void initGDTR();
void setPageMap(void* map);
void enableSSE();
std::uint8_t readIO8(std::uint16_t addr);
std::uint16_t readIO16(std::uint16_t addr);
std::uint32_t readIO32(std::uint16_t addr);
void writeIO8(std::uint16_t addr, std::uint8_t value);
void writeIO16(std::uint16_t addr, std::uint16_t value);
void writeIO32(std::uint16_t addr, std::uint32_t value);
}
}  // namespace oz