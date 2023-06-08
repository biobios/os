#include "utils.hpp"

void oz::utils::to_hex(std::uint64_t num, char* str) {
    std::int64_t i;
    std::uint64_t halfByte;
    for(i = 2*sizeof(num) - 1; i >= 0; i--){
        halfByte = num & 0x000000000000000F;
        if(halfByte >= 10){
            str[i] = ('A' + halfByte - 0xA);
        }else{
            str[i] = ('0' + halfByte);
        }
        num = num >> 4;
    }

    str[2*sizeof(num)] = '\0';
}