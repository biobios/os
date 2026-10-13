#pragma once
#include "core/Capability.hpp"
#include <cstdint>

namespace oz {

// IOPort: capability-based I/O port access object (x86-specific)
// Grants access to a range of I/O ports via capability rights.
class IOPort : public KernelObject {
public:
    // Function pointers for actual I/O operations (set by Kernel.hpp)
    using ReadFn8 = std::uint8_t (*)(std::uint16_t);
    using ReadFn16 = std::uint16_t (*)(std::uint16_t);
    using ReadFn32 = std::uint32_t (*)(std::uint16_t);
    using WriteFn8 = void (*)(std::uint16_t, std::uint8_t);
    using WriteFn16 = void (*)(std::uint16_t, std::uint16_t);
    using WriteFn32 = void (*)(std::uint16_t, std::uint32_t);

private:
    std::uint16_t base_port_;
    std::uint16_t port_count_;

    ReadFn8 read8_fn_;
    ReadFn16 read16_fn_;
    ReadFn32 read32_fn_;
    WriteFn8 write8_fn_;
    WriteFn16 write16_fn_;
    WriteFn32 write32_fn_;

public:
    IOPort(std::uint16_t base, std::uint16_t count,
           ReadFn8 r8, ReadFn16 r16, ReadFn32 r32,
           WriteFn8 w8, WriteFn16 w16, WriteFn32 w32)
        : KernelObject(ObjectType::IOPort),
          base_port_(base), port_count_(count),
          read8_fn_(r8), read16_fn_(r16), read32_fn_(r32),
          write8_fn_(w8), write16_fn_(w16), write32_fn_(w32) {}

    std::uint16_t basePort() const { return base_port_; }
    std::uint16_t portCount() const { return port_count_; }

    bool isInRange(std::uint16_t port) const {
        return port >= base_port_ && port < base_port_ + port_count_;
    }

    // Read operations (check range)
    bool read8(std::uint16_t port, std::uint8_t& out) const {
        if (!isInRange(port) || !read8_fn_) return false;
        out = read8_fn_(port);
        return true;
    }

    bool read16(std::uint16_t port, std::uint16_t& out) const {
        if (!isInRange(port) || !read16_fn_) return false;
        out = read16_fn_(port);
        return true;
    }

    bool read32(std::uint16_t port, std::uint32_t& out) const {
        if (!isInRange(port) || !read32_fn_) return false;
        out = read32_fn_(port);
        return true;
    }

    // Write operations (check range)
    bool write8(std::uint16_t port, std::uint8_t value) {
        if (!isInRange(port) || !write8_fn_) return false;
        write8_fn_(port, value);
        return true;
    }

    bool write16(std::uint16_t port, std::uint16_t value) {
        if (!isInRange(port) || !write16_fn_) return false;
        write16_fn_(port, value);
        return true;
    }

    bool write32(std::uint16_t port, std::uint32_t value) {
        if (!isInRange(port) || !write32_fn_) return false;
        write32_fn_(port, value);
        return true;
    }
};

} // namespace oz
