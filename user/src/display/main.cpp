#include "syscall.hpp"

namespace {

struct Pixel {
    std::uint8_t b;
    std::uint8_t g;
    std::uint8_t r;
    std::uint8_t a;
};

static constexpr std::uint32_t CAP_CONSOLE_SEND = 1;
static constexpr std::uint32_t CAP_DISPLAY_RECV = 2;
static constexpr std::uint64_t FB_VADDR = 0x650000000000ULL;

class DisplayDriver {
private:
    Pixel* fb_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::size_t size_ = 0;

public:
    DisplayDriver() = default;

    bool init(std::uint32_t width, std::uint32_t height, std::size_t size) {
        width_ = width;
        height_ = height;
        size_ = size;

        // Map FrameBuffer capability (Slot 246) into self VSpace at FB_VADDR
        // flags: 0x01 (Writable)
        int res = oz::sys_vspace_map(oz::CAP_SELF_VSPACE, oz::CAP_FRAMEBUFFER, FB_VADDR, 0x01);
        if (res != 0) {
            oz::sys_debug_print("[Display] ERROR: Failed to map FrameBuffer!\n\r");
            return false;
        }

        fb_ = reinterpret_cast<Pixel*>(FB_VADDR);
        oz::sys_debug_print("[Display] FrameBuffer mapped successfully.\n\r");
        return true;
    }

    void putPixel(std::uint32_t x, std::uint32_t y, Pixel color) {
        if (x >= width_ || y >= height_) return;
        fb_[y * width_ + x] = color;
    }

    void fillRect(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, Pixel color) {
        std::uint32_t x_end = (x + w > width_) ? width_ : x + w;
        std::uint32_t y_end = (y + h > height_) ? height_ : y + h;

        for (std::uint32_t cy = y; cy < y_end; ++cy) {
            for (std::uint32_t cx = x; cx < x_end; ++cx) {
                fb_[cy * width_ + cx] = color;
            }
        }
    }

    void clear(Pixel color) {
        fillRect(0, 0, width_, height_, color);
    }

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
};

static DisplayDriver g_display;

// Simple test GUI rendering
void renderTestUI(DisplayDriver& disp) {
    // 1. Dark Slate Blue Background
    disp.clear(Pixel{0x36, 0x22, 0x1e, 0}); // #1e2236 (BGR: 0x36, 0x22, 0x1e)

    // 2. Draw a desktop panel / top bar (Dark Grey)
    disp.fillRect(0, 0, disp.width(), 32, Pixel{0x2b, 0x24, 0x1f, 0});

    // 3. Draw a sample Window in the center
    std::uint32_t win_w = 400;
    std::uint32_t win_h = 250;
    std::uint32_t win_x = (disp.width() > win_w) ? (disp.width() - win_w) / 2 : 50;
    std::uint32_t win_y = (disp.height() > win_h) ? (disp.height() - win_h) / 2 : 50;

    // Window shadow
    disp.fillRect(win_x + 4, win_y + 4, win_w, win_h, Pixel{0x10, 0x10, 0x10, 0});
    // Window background (Light Slate)
    disp.fillRect(win_x, win_y, win_w, win_h, Pixel{0x4a, 0x3b, 0x2e, 0});
    // Window title bar (Accent Blue)
    disp.fillRect(win_x, win_y, win_w, 28, Pixel{0x9e, 0x5a, 0x24, 0});

    // Window close button (Red dot)
    disp.fillRect(win_x + 10, win_y + 8, 12, 12, Pixel{0x40, 0x40, 0xe0, 0});
    // Window minimize button (Yellow dot)
    disp.fillRect(win_x + 28, win_y + 8, 12, 12, Pixel{0x30, 0xc0, 0xe0, 0});
    // Window maximize button (Green dot)
    disp.fillRect(win_x + 46, win_y + 8, 12, 12, Pixel{0x40, 0xd0, 0x40, 0});

    // Content area inside window
    disp.fillRect(win_x + 15, win_y + 45, win_w - 30, win_h - 60, Pixel{0x20, 0x18, 0x12, 0});
}

} // namespace

extern "C" void _start() {
    oz::sys_debug_print("[Display] Display Server started (Ring 3)!\n\r");

    // Wait for configuration IPC message from init process on Channel 2
    oz::IPCMessage config_msg{};
    if (oz::sys_channel_recv(CAP_DISPLAY_RECV, &config_msg)) {
        std::uint32_t width = static_cast<std::uint32_t>(config_msg.data[0]);
        std::uint32_t height = static_cast<std::uint32_t>(config_msg.data[1]);
        std::size_t size = static_cast<std::size_t>(config_msg.data[2]);

        oz::sys_debug_print("[Display] Received config from init.\n\r");

        if (g_display.init(width, height, size)) {
            oz::sys_debug_print("[Display] Rendering initial GUI scene...\n\r");
            renderTestUI(g_display);
            oz::sys_debug_print("[Display] Initial GUI scene rendered!\n\r");
        }
    } else {
        oz::sys_debug_print("[Display] Failed to receive config message!\n\r");
    }

    // Main event loop for IPC requests
    oz::IPCMessage msg{};
    while (true) {
        if (oz::sys_channel_recv(CAP_DISPLAY_RECV, &msg)) {
            // Future: Handle window creation, draw commands, input events
        }
    }
}
