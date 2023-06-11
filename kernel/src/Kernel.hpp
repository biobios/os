#pragma once

#include <cstdint>

#include "Graphics.hpp"
#include "Shell.hpp"
#include "PageManager.hpp"
#include "bootStructures.hpp"
#include "MemoryManager.hpp"

namespace oz {
class Kernel {
    public:
    Graphics g;
    Shell sh;
    private:
    PageManager pm;
    MemoryManager<binary_units::operator""_Gi(128)> mm;
   public:
    Kernel(oz_boot::PlatformInfo* platformInfo);
    void run();
};
}  // namespace oz