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

void setKernelPtr(void* k);

void dprint(const char* str);

void write(const char* str);

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