#include "hardware/Serial.hpp"
#include "hardware/x86_64.hpp"

namespace oz {
namespace hardware {

constexpr std::uint16_t COM1_PORT = 0x3f8;

void Serial::init() {
    oz::x86_64::writeIO8(COM1_PORT + 1, 0x00); // Disable all interrupts
    oz::x86_64::writeIO8(COM1_PORT + 3, 0x80); // Enable DLAB (set baud rate divisor)
    oz::x86_64::writeIO8(COM1_PORT + 0, 0x01); // Set divisor to 1 (lo byte) 115200 baud
    oz::x86_64::writeIO8(COM1_PORT + 1, 0x00); //                  (hi byte)
    oz::x86_64::writeIO8(COM1_PORT + 3, 0x03); // 8 bits, no parity, one stop bit
    oz::x86_64::writeIO8(COM1_PORT + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    oz::x86_64::writeIO8(COM1_PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

void Serial::writeChar(char c) {
    // Wait until the transmit buffer is empty
    while ((oz::x86_64::readIO8(COM1_PORT + 5) & 0x20) == 0) {
        // wait
    }
    oz::x86_64::writeIO8(COM1_PORT, c);
}

void Serial::writeString(const char* str) {
    while (*str) {
        writeChar(*str);
        str++;
    }
}

} // namespace hardware
} // namespace oz
