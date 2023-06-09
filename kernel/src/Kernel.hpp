#pragma once

#include <cstdint>

#include "Graphics.hpp"
#include "Shell.hpp"
#include "PageManager.hpp"
#include "bootStructures.hpp"

void setKernelPtr(void* k);
void dprint(const char* str);

namespace oz {
class Kernel {
    Graphics g;
    public:
    Shell sh;
    private:
    PageManager pm;
   public:
    Kernel(oz_boot::PlatformInfo* platformInfo);
    void run();
};
}  // namespace oz