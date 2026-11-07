#pragma once

#include <cstdint>
#include <cstddef>

#include "graphics/Graphics.hpp"

namespace oz
{
    class Shell
    {
    public:
        static constexpr std::size_t LETTERS_NUM = 20000;
        static constexpr std::uint32_t LINE_SPACING = 5;
        static constexpr std::uint32_t LETTER_SPACING = 3;

    private:
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