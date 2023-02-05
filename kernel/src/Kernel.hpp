#pragma once

#include <cstdint>

#include "Graphics.hpp"
#include "Shell.hpp"

struct PlatformInfo {
    void* frame_buffer_base;
    std::uint64_t frame_buffer_size;
    std::uint32_t frame_buffer_horizontal;
    std::uint32_t frame_buffer_vertical;
    void* RSDP;
};

namespace oz {
class Kernel {
    Graphics g;
    Shell sh;

   public:
    Kernel(PlatformInfo* platformInfo);
    void run();
};
}  // namespace oz