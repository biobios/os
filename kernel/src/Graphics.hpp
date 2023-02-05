#pragma once

#include <cstdint>

struct Pixel {
    std::uint8_t b;
    std::uint8_t g;
    std::uint8_t r;
    std::uint8_t reserved;
};

namespace oz {
class Graphics {
    Pixel* buffer_base;
    std::uint64_t buffer_size;
    std::uint32_t horizontal_length;
    std::uint32_t vertical_length;
    Pixel current_color;
    Pixel background_color;

   public:
    Graphics(Pixel* frame_buffer_base, std::uint64_t frame_buffer_size,
             std::uint32_t fb_horizontal_length, std::uint32_t fb_vertical_length);
    void setPixel(const Pixel& color, std::uint32_t x, std::uint32_t y);
    void setColor(const Pixel& color);
    void setBackground(const Pixel& color);
    void fillRect(std::uint32_t base_x, std::uint32_t base_y, std::uint32_t width,
                  std::uint32_t height);
    void clearScreen();
    void drawCharacter(std::int8_t c, std::uint32_t base_x, std::uint32_t base_y);
    std::uint32_t getWidth();
    std::uint32_t getHeight();
};
}  // namespace oz