#include "utils.hpp"
#include "Kernel.hpp"

namespace {
    oz::Kernel* kernelPtr;
}

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

void setKernelPtr(void* k) {
    kernelPtr = reinterpret_cast<oz::Kernel*>(k);
}

void dprint(const char* str) { kernelPtr->sh.printString(str); }

void write(const char* str) {
    kernelPtr->g.clearScreen();
    
    std::uint64_t x = 0;
    while(*str != '\0'){
        switch (*str)
        {
        case '\r':
        case '\n':
            break;
        default:
            if((*str < ' ') || (*str > '~'))break;
            kernelPtr->g.drawCharacter(*str, x, 0);
            x += 8 + LETTER_SPACING;
            break;
        }
        str++;
    }
}

void abort() {

    dprint("abort");

    kernelPtr->sh.repaint();

    while(true){
        __asm__ volatile("hlt");
    }
}
