#pragma once

#include <cstdint>

#include "Graphics.hpp"

#define LETTERS_NUM 20000
#define LINE_SPACING 5
#define LETTER_SPACING 3

namespace oz
{
    class Shell
    {
        Graphics* gc;
        std::uint8_t string[LETTERS_NUM];
        std::uint32_t nextIndex;
        std::uint32_t scissorRectTop;
        std::uint32_t currentCursorX;
        std::uint32_t currentCursorY;
    public:
        Shell(Graphics* graphics);
        void printString(const char* str);
        void repaint();
    };
}