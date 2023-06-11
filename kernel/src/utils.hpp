#pragma once

#include <cstdint>

namespace binary_units{
    constexpr std::uint64_t operator"" _Ki (std::uint64_t num){
        return num * 1024;
    }

    constexpr std::uint64_t operator"" _Mi (std::uint64_t num){
        return num * 1024_Ki;
    }

    constexpr std::uint64_t operator"" _Gi (std::uint64_t num){
        return num * 1024_Mi;
    }

}

void setKernelPtr(void* k);

void dprint(const char* str);

void write(const char* str);

namespace oz{
    namespace utils{
        void to_hex(std::uint64_t num, char* str);
    }
}