#include "Shell.hpp"
#include "Font.hpp"

oz::Shell::Shell(Graphics* graphics)
{
    gc = graphics;
    nextIndex = 0;
    scissorRectTop = 0;
    currentCursorX = 0;
    currentCursorY = 0;
}

void oz::Shell::printString(const char* str)
{
    while(*str != '\0'){
        if((currentCursorY + FONT_HEIGHT - scissorRectTop) >= gc->getHeight()){
            scissorRectTop = currentCursorY + FONT_HEIGHT - gc->getHeight();
        }
        switch (*str)
        {
        case '\r':
            currentCursorX = 0;
            string[nextIndex] = *str;
            nextIndex++;
            break;
        case '\n':
            currentCursorY += FONT_HEIGHT + LINE_SPACING;
            string[nextIndex] = *str;
            nextIndex++;
            break;
        default:
            if((*str < ' ') || (*str > '~'))break;
            gc->drawCharacter(*str, currentCursorX, currentCursorY - scissorRectTop);
            currentCursorX += FONT_WIDTH + LETTER_SPACING;
            string[nextIndex] = *str;
            nextIndex++;
            break;
        }
        str++;
    }
}

void oz::Shell::repaint()
{
    gc->clearScreen();
    std::uint32_t index;
    currentCursorX = 0;
    currentCursorY = 0;
    for(index = 0; index < nextIndex; index++){
        switch(string[index])
        {
        case '\r':
            currentCursorX = 0;
            break;
        case '\n':
            currentCursorY += FONT_HEIGHT + LINE_SPACING;
            break;
        default:
            if(currentCursorY < scissorRectTop)break;
            gc->drawCharacter(string[index], currentCursorX, currentCursorY - scissorRectTop);
            currentCursorX += FONT_WIDTH + LETTER_SPACING;
            break;
        }
    }
}