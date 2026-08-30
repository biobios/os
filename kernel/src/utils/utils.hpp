#pragma once

#include <cstdint>
#include <cstddef>

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
namespace oz {
    class Shell;
}

void setShellPtr(oz::Shell* k);

void dprint(const char* str);

// void write(const char* str);

void abort();

namespace oz{
    namespace utils{
        void to_hex(std::uint64_t num, char* str);
        constexpr std::uint64_t exponentiate(std::uint64_t base, std::uint64_t exponent){
            std::uint64_t result = 1;
            for(std::size_t i = 0; i < exponent; i++){
                result *= base;
            }
            return result;
        }
        constexpr std::uint64_t getMSB(std::uint64_t num){
            if(num == 0){
                return ~(0);
            }
            std::size_t i;
            for(i = 63; true; i--){
                if(num >> i != 0){
                    break;
                }
            }
            return i;
        }
        constexpr std::uint64_t getLSB(std::uint64_t num){
            if(num == 0){
                return ~(0);
            }
            std::size_t i;
            for(i = 0; true; i++){
                if((num << (63 - i)) != 0){
                    break;
                }
            }
            return i;
        }
        constexpr bool isPowerOf2(std::uint64_t num){
            std::uint64_t setBitCount = 0;
            for(std::size_t i = 0; i < 64; i++){
                setBitCount += 0b1 & num >> i;
            }
            return setBitCount = 1;
        }
    }
}
namespace oz {
namespace utils {
    inline void memcpy(void* dest, const void* src, std::size_t count) {
        char* d = static_cast<char*>(dest);
        const char* s = static_cast<const char*>(src);
        while (count--) {
            *d++ = *s++;
        }
    }
    inline void memset(void* dest, std::uint8_t val, std::size_t count) {
        std::uint8_t* d = static_cast<std::uint8_t*>(dest);
        while (count--) {
            *d++ = val;
        }
    }
    inline int memcmp(const void* lhs, const void* rhs, std::size_t count) {
        const unsigned char* p1 = static_cast<const unsigned char*>(lhs);
        const unsigned char* p2 = static_cast<const unsigned char*>(rhs);
        while (count--) {
            if (*p1 != *p2) return *p1 - *p2;
            p1++; p2++;
        }
        return 0;
    }
    inline char toupper(char c) {
        if (c >= 'a' && c <= 'z') return c - 32;
        return c;
    }
}
}
