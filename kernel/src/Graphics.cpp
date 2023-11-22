#include "Graphics.hpp"

#include "Font.hpp"

oz::Graphics::Graphics(Pixel* frame_buffer_base,
                       std::uint64_t frame_buffer_size,
                       std::uint32_t fb_horizontal_length,
                       std::uint32_t fb_vertical_length) {
    buffer_base = frame_buffer_base;
    buffer_size = frame_buffer_size;
    horizontal_length = fb_horizontal_length;
    vertical_length = fb_vertical_length;
    current_color.b = 255;
    current_color.g = 255;
    current_color.r = 255;
    background_color.b = 40;
    background_color.g = 40;
    background_color.r = 40;
}

void oz::Graphics::setPixel(const Pixel& color, std::uint32_t x,
                            std::uint32_t y) {
    if ((x >= horizontal_length) || (y >= vertical_length)) return;
    Pixel* p = buffer_base + (horizontal_length * y) + x;
    p->b = color.b;
    p->g = color.g;
    p->r = color.r;
}

void oz::Graphics::setColor(const Pixel& color) { current_color = color; }

void oz::Graphics::setBackground(const Pixel& color) {
    background_color = color;
}

void oz::Graphics::fillRect(std::uint32_t base_x, std::uint32_t base_y,
                            std::uint32_t width, std::uint32_t height) {
    if (base_x >= horizontal_length) {
        return;
    } else if (base_x + width > horizontal_length) {
        width = horizontal_length - base_x;
    }
    if (base_y >= vertical_length) {
        return;
    } else if (base_y + height > vertical_length) {
        height = vertical_length - base_y;
    }
    std::uint32_t x;
    std::uint32_t y;
    for (y = 0; y < base_y + height; y++) {
        for (x = 0; x < base_x + width; x++) {
            setPixel(current_color, x, y);
        }
    }
}

void oz::Graphics::clearScreen() {
    std::uint32_t x;
    std::uint32_t y;
    for (y = 0; y < vertical_length; y++) {
        for (x = 0; x < horizontal_length; x++) {
            setPixel(background_color, x, y);
        }
    }
}

void oz::Graphics::drawCharacter(std::int8_t c, std::uint32_t base_x,
                                 std::uint32_t base_y) {
    if ((base_x >= horizontal_length) || (base_y >= vertical_length)) return;
    if ((c < ' ') || (c > '~')) return;
    std::uint32_t x;
    std::uint32_t y;
    for (y = 0; y < FONT_HEIGHT; y++) {
        for (x = 0; x < FONT_WIDTH; x++) {
            if (font_bitmap[c - 32][y][x]) {
                setPixel(current_color, base_x + x, base_y + y);
            }
        }
    }
}

std::uint32_t oz::Graphics::getWidth() { return horizontal_length; }

std::uint32_t oz::Graphics::getHeight() { return vertical_length; }