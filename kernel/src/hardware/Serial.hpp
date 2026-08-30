#pragma once

#include <cstdint>
#include <cstddef>

namespace oz {
namespace hardware {

class Serial {
public:
    static void init();
    static void writeChar(char c);
    static void writeString(const char* str);
};

} // namespace hardware
} // namespace oz
