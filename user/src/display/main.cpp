#include "syscall.hpp"
#include "Font.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/utils/utils.hpp"

namespace {

struct Pixel {
    std::uint8_t b;
    std::uint8_t g;
    std::uint8_t r;
    std::uint8_t a;
};

static constexpr std::uint32_t CAP_CONSOLE_SEND = 1;
static constexpr std::uint32_t CAP_DISPLAY_RECV = 2;
static constexpr std::uint32_t CAP_COMMAND_RECV = 3;
static constexpr std::uint64_t FB_VADDR = 0x650000000000ULL;
static constexpr std::uint64_t BACK_BUF_VADDR = 0x660000000000ULL;

class DisplayDriver {
private:
    Pixel* fb_ = nullptr;          // Physical VRAM
    Pixel* back_buffer_ = nullptr; // Off-screen RAM back buffer
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::size_t size_ = 0;

public:
    DisplayDriver() = default;

    bool init(std::uint32_t width, std::uint32_t height, std::size_t size) {
        width_ = width;
        height_ = height;
        size_ = size;

        // 1. Map physical FrameBuffer (VRAM)
        int res = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_FRAMEBUFFER, FB_VADDR, 0x01, size);
        if (res != 0) {
            oz::sys_debug_print("[Display] ERROR: Failed to map FrameBuffer!\n\r");
            return false;
        }
        fb_ = reinterpret_cast<Pixel*>(FB_VADDR);

        // 2. Allocate and map BackBuffer (RAM)
        std::uint64_t num_pages = (size + 4095) / 4096;
        std::uint32_t back_cap = oz::sys_frame_alloc(oz::CAP_UNTYPED, num_pages);
        if (back_cap == oz::INVALID_CAP_ID) {
            oz::sys_debug_print("[Display] ERROR: Failed to allocate BackBuffer frame!\n\r");
            return false;
        }
        res = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, back_cap, BACK_BUF_VADDR, 0x01, size);
        if (res != 0) {
            oz::sys_debug_print("[Display] ERROR: Failed to map BackBuffer!\n\r");
            return false;
        }
        back_buffer_ = reinterpret_cast<Pixel*>(BACK_BUF_VADDR);

        oz::sys_debug_print("[Display] FrameBuffer & BackBuffer mapped successfully.\n\r");
        return true;
    }

    void putPixel(std::uint32_t x, std::uint32_t y, Pixel color) {
        if (!back_buffer_ || x >= width_ || y >= height_) return;
        back_buffer_[y * width_ + x] = color;
    }

    void fillRect(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h, Pixel color) {
        if (!back_buffer_ || w == 0 || h == 0) return;

        if (x < 0) {
            if (static_cast<std::int32_t>(w) <= -x) return;
            w = static_cast<std::uint32_t>(static_cast<std::int32_t>(w) + x);
            x = 0;
        }
        if (y < 0) {
            if (static_cast<std::int32_t>(h) <= -y) return;
            h = static_cast<std::uint32_t>(static_cast<std::int32_t>(h) + y);
            y = 0;
        }
        if (static_cast<std::uint32_t>(x) >= width_ || static_cast<std::uint32_t>(y) >= height_) return;
        if (static_cast<std::uint32_t>(x) + w > width_) w = width_ - static_cast<std::uint32_t>(x);
        if (static_cast<std::uint32_t>(y) + h > height_) h = height_ - static_cast<std::uint32_t>(y);
        if (w == 0 || h == 0) return;

        std::uint32_t ux = static_cast<std::uint32_t>(x);
        std::uint32_t uy = static_cast<std::uint32_t>(y);
        for (std::uint32_t cy = uy; cy < uy + h; ++cy) {
            for (std::uint32_t cx = ux; cx < ux + w; ++cx) {
                back_buffer_[cy * width_ + cx] = color;
            }
        }
    }

    void clear(Pixel color) {
        fillRect(0, 0, width_, height_, color);
    }

    void blit(std::int32_t dst_x, std::int32_t dst_y, const Pixel* src, std::uint32_t src_w, std::uint32_t src_h,
              std::uint32_t max_w = 0, std::uint32_t max_h = 0) {
        if (!back_buffer_ || !src) return;

        std::int32_t start_x = (dst_x < 0) ? -dst_x : 0;
        std::int32_t start_y = (dst_y < 0) ? -dst_y : 0;

        std::int32_t end_x = static_cast<std::int32_t>(src_w);
        if (max_w > 0 && end_x > static_cast<std::int32_t>(max_w)) {
            end_x = static_cast<std::int32_t>(max_w);
        }
        if (dst_x + end_x > static_cast<std::int32_t>(width_)) {
            end_x = static_cast<std::int32_t>(width_) - dst_x;
        }
        std::int32_t end_y = static_cast<std::int32_t>(src_h);
        if (max_h > 0 && end_y > static_cast<std::int32_t>(max_h)) {
            end_y = static_cast<std::int32_t>(max_h);
        }
        if (dst_y + end_y > static_cast<std::int32_t>(height_)) {
            end_y = static_cast<std::int32_t>(height_) - dst_y;
        }

        std::int32_t copy_w = end_x - start_x;
        if (copy_w <= 0) return;

        for (std::int32_t sy = start_y; sy < end_y; ++sy) {
            std::int32_t dy = dst_y + sy;
            if (dy < 0 || dy >= static_cast<std::int32_t>(height_)) continue;
            std::size_t dst_offset = static_cast<std::size_t>(dy) * width_ + static_cast<std::size_t>(dst_x + start_x);
            std::size_t src_offset = static_cast<std::size_t>(sy) * src_w + static_cast<std::size_t>(start_x);
            __builtin_memcpy(&back_buffer_[dst_offset], &src[src_offset], static_cast<std::size_t>(copy_w) * sizeof(Pixel));
        }
    }

    void drawChar(std::uint32_t x, std::uint32_t y, char c, Pixel fg) {
        if (!back_buffer_ || c < 32 || c > 126) return;
        if (x + FONT_WIDTH > width_ || y + FONT_HEIGHT > height_) return;

        for (int r = 0; r < FONT_HEIGHT; ++r) {
            for (int col = 0; col < FONT_WIDTH; ++col) {
                if (font_bitmap[c - 32][r][col]) {
                    back_buffer_[(y + r) * width_ + (x + col)] = fg;
                }
            }
        }
    }

    void drawString(std::uint32_t x, std::uint32_t y, const char* str, Pixel fg) {
        while (*str) {
            drawChar(x, y, *str, fg);
            x += FONT_WIDTH;
            str++;
        }
    }

    void scrollRegion(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint32_t lines, Pixel bg) {
        if (!back_buffer_) return;
        if (lines >= h) {
            fillRect(x, y, w, h, bg);
            return;
        }
        for (std::uint32_t cy = y; cy < y + h - lines; ++cy) {
            for (std::uint32_t cx = x; cx < x + w; ++cx) {
                back_buffer_[cy * width_ + cx] = back_buffer_[(cy + lines) * width_ + cx];
            }
        }
        fillRect(x, y + h - lines, w, lines, bg);
    }

    // High-speed scanline flush from BackBuffer to physical VRAM
    void flush(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) {
        if (!fb_ || !back_buffer_) return;

        if (x < 0) {
            if (static_cast<std::int32_t>(w) <= -x) return;
            w = static_cast<std::uint32_t>(static_cast<std::int32_t>(w) + x);
            x = 0;
        }
        if (y < 0) {
            if (static_cast<std::int32_t>(h) <= -y) return;
            h = static_cast<std::uint32_t>(static_cast<std::int32_t>(h) + y);
            y = 0;
        }
        if (static_cast<std::uint32_t>(x) >= width_ || static_cast<std::uint32_t>(y) >= height_) return;
        if (static_cast<std::uint32_t>(x) + w > width_) w = width_ - static_cast<std::uint32_t>(x);
        if (static_cast<std::uint32_t>(y) + h > height_) h = height_ - static_cast<std::uint32_t>(y);
        if (w == 0 || h == 0) return;

        for (std::uint32_t r = 0; r < h; ++r) {
            std::size_t offset = static_cast<std::size_t>(static_cast<std::uint32_t>(y) + r) * width_ + static_cast<std::uint32_t>(x);
            Pixel* dst = fb_ + offset;
            const Pixel* src = back_buffer_ + offset;
            __builtin_memcpy(dst, src, static_cast<std::size_t>(w) * sizeof(Pixel));
        }
    }

    void flushAll() {
        flush(0, 0, width_, height_);
    }

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
};


static DisplayDriver g_display;

// GUI Terminal Console embedded inside a Window
class TerminalConsole {
private:
    DisplayDriver* disp_{nullptr};
    std::uint32_t win_x_{0};
    std::uint32_t win_y_{0};
    std::uint32_t win_w_{0};
    std::uint32_t win_h_{0};

    std::uint32_t cx_{0};
    std::uint32_t cy_{0};
    std::uint32_t cw_{0};
    std::uint32_t ch_{0};

    std::uint32_t cursor_col_{0};
    std::uint32_t cursor_row_{0};
    std::uint32_t max_cols_{0};
    std::uint32_t max_rows_{0};

    Pixel fg_color_{0xe0, 0xe0, 0xe0, 0}; // Light Grey / White
    Pixel bg_color_{0x1e, 0x16, 0x12, 0}; // Dark Slate

public:
    void init(DisplayDriver* disp, std::uint32_t wx, std::uint32_t wy, std::uint32_t ww, std::uint32_t wh) {
        disp_ = disp;
        win_x_ = wx;
        win_y_ = wy;
        win_w_ = ww;
        win_h_ = wh;

        // Content area inside window frame
        cx_ = win_x_ + 10;
        cy_ = win_y_ + 38;
        cw_ = win_w_ - 20;
        ch_ = win_h_ - 48;

        max_cols_ = cw_ / FONT_WIDTH;
        max_rows_ = ch_ / FONT_HEIGHT;
        cursor_col_ = 0;
        cursor_row_ = 0;

        renderWindowFrame();
    }

    void renderWindowFrame() {
        if (!disp_) return;

        // Window shadow
        disp_->fillRect(win_x_ + 4, win_y_ + 4, win_w_, win_h_, Pixel{0x10, 0x10, 0x10, 0});
        // Window background
        disp_->fillRect(win_x_, win_y_, win_w_, win_h_, Pixel{0x35, 0x2b, 0x22, 0});
        // Window title bar (Deep Slate Blue)
        disp_->fillRect(win_x_, win_y_, win_w_, 30, Pixel{0x7a, 0x48, 0x22, 0});

        // Window control buttons
        disp_->fillRect(win_x_ + 10, win_y_ + 9, 12, 12, Pixel{0x40, 0x40, 0xe0, 0}); // Close (Red)
        disp_->fillRect(win_x_ + 28, win_y_ + 9, 12, 12, Pixel{0x30, 0xc0, 0xe0, 0}); // Minimize (Yellow)
        disp_->fillRect(win_x_ + 46, win_y_ + 9, 12, 12, Pixel{0x40, 0xd0, 0x40, 0}); // Maximize (Green)

        // Title text
        disp_->drawString(win_x_ + 70, win_y_ + 7, "OZ OS Terminal", Pixel{0xff, 0xff, 0xff, 0});

        // Content area
        disp_->fillRect(cx_, cy_, cw_, ch_, bg_color_);
    }

    void clear() {
        if (!disp_) return;
        disp_->fillRect(cx_, cy_, cw_, ch_, bg_color_);
        cursor_col_ = 0;
        cursor_row_ = 0;
    }

    void newLine() {
        cursor_col_ = 0;
        cursor_row_++;
        if (cursor_row_ >= max_rows_) {
            disp_->scrollRegion(cx_, cy_, cw_, ch_, FONT_HEIGHT, bg_color_);
            cursor_row_ = max_rows_ - 1;
        }
    }

    void putChar(char c) {
        if (!disp_) return;

        if (c == '\n') {
            newLine();
            return;
        }
        if (c == '\r') {
            cursor_col_ = 0;
            return;
        }
        if (c == '\b' || c == 0x7F) {
            if (cursor_col_ > 0) {
                cursor_col_--;
                disp_->fillRect(cx_ + cursor_col_ * FONT_WIDTH, cy_ + cursor_row_ * FONT_HEIGHT, FONT_WIDTH, FONT_HEIGHT, bg_color_);
            }
            return;
        }
        if (c < 32 || c > 126) return;

        if (cursor_col_ >= max_cols_) {
            newLine();
        }

        // Clear cell before drawing char
        disp_->fillRect(cx_ + cursor_col_ * FONT_WIDTH, cy_ + cursor_row_ * FONT_HEIGHT, FONT_WIDTH, FONT_HEIGHT, bg_color_);
        disp_->drawChar(cx_ + cursor_col_ * FONT_WIDTH, cy_ + cursor_row_ * FONT_HEIGHT, c, fg_color_);
        cursor_col_++;
    }

    void write(const char* str) {
        // Simple ANSI escape sequence handling for clear screen
        while (*str) {
            if (str[0] == '\033' && str[1] == '[' && str[2] == '2' && str[3] == 'J') {
                clear();
                str += 4;
                continue;
            }
            if (str[0] == '\033' && str[1] == '[' && str[2] == 'H') {
                cursor_col_ = 0;
                cursor_row_ = 0;
                str += 3;
                continue;
            }
            putChar(*str);
            str++;
        }
    }
};

static TerminalConsole g_terminal;

void renderDesktop(DisplayDriver& disp) {
    // 1. Dark Slate Blue Desktop Background
    disp.clear(Pixel{0x36, 0x22, 0x1e, 0});

    // 2. Top Bar
    disp.fillRect(0, 0, disp.width(), 28, Pixel{0x2b, 0x24, 0x1f, 0});
    disp.drawString(12, 6, "OZ Operating System (Capability-Based Microkernel)", Pixel{0xd0, 0xd0, 0xd0, 0});
}

void drawCursor(DisplayDriver& disp, std::int32_t x, std::int32_t y) {

    static const char* cursor_pattern[16] = {
        "X          ",
        "XX         ",
        "X.X        ",
        "X..X       ",
        "X...X      ",
        "X....X     ",
        "X.....X    ",
        "X......X   ",
        "X.......X  ",
        "X.....XXXX ",
        "X..X..X    ",
        "X.X X..X   ",
        "XX   X..X  ",
        "X     X..X ",
        "       XX  ",
        "           ",
    };
    Pixel black{0, 0, 0, 0};
    Pixel white{255, 255, 255, 0};

    for (int r = 0; r < 16; ++r) {
        for (int c = 0; c < 11; ++c) {
            char p = cursor_pattern[r][c];
            if (p == 'X') {
                disp.putPixel(x + c, y + r, black);
            } else if (p == '.') {
                disp.putPixel(x + c, y + r, white);
            }
        }
    }
}

static constexpr std::uint64_t SURFACE_BUF_BASE_VIRT = 0x400000000000ULL;
static constexpr std::size_t MAX_SURFACES = 16;

struct Surface {
    std::uint32_t id{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t rect_w{0};
    std::uint32_t rect_h{0};
    std::int32_t x{0};
    std::int32_t y{0};
    bool visible{false};
    std::int32_t z_order{0};
    std::uint32_t frame_cap{oz::INVALID_CAP_ID};
    Pixel* buffer{nullptr};
    std::size_t size{0};
    std::uint32_t border_width{0};
    Pixel border_color{0, 0, 0, 0};
};

static Surface g_surfaces[MAX_SURFACES] = {};
static std::int32_t g_cursor_x = 200;
static std::int32_t g_cursor_y = 150;
static bool g_cursor_visible = true;

bool has_active_surfaces() {
    for (std::size_t i = 0; i < MAX_SURFACES; ++i) {
        if (g_surfaces[i].id != 0 && g_surfaces[i].visible && g_surfaces[i].buffer) {
            return true;
        }
    }
    return false;
}

void renderSurface(DisplayDriver& disp, const Surface& surf, bool render_border = true) {
    if (surf.id == 0 || !surf.visible || !surf.buffer) return;

    std::uint32_t draw_w = (surf.rect_w > 0) ? surf.rect_w : surf.width;
    std::uint32_t draw_h = (surf.rect_h > 0) ? surf.rect_h : surf.height;

    // 1. Draw 4 border edges only (never overwrite the interior window area)
    if (render_border && surf.border_width > 0) {
        std::int32_t bw = static_cast<std::int32_t>(surf.border_width);
        // Top edge
        disp.fillRect(surf.x - bw, surf.y - bw, draw_w + 2 * bw, bw, surf.border_color);
        // Bottom edge
        disp.fillRect(surf.x - bw, surf.y + static_cast<std::int32_t>(draw_h), draw_w + 2 * bw, bw, surf.border_color);
        // Left edge
        disp.fillRect(surf.x - bw, surf.y, bw, draw_h, surf.border_color);
        // Right edge
        disp.fillRect(surf.x + static_cast<std::int32_t>(draw_w), surf.y, bw, draw_h, surf.border_color);
    }

    // 2. Fill background only if surface buffer is smaller than target rect (unused margins)
    if (surf.width < draw_w) {
        disp.fillRect(surf.x + static_cast<std::int32_t>(surf.width), surf.y, draw_w - surf.width, draw_h, Pixel{0x10, 0x10, 0x10, 0});
    }
    if (surf.height < draw_h) {
        std::uint32_t fill_w = (surf.width < draw_w) ? surf.width : draw_w;
        disp.fillRect(surf.x, surf.y + static_cast<std::int32_t>(surf.height), fill_w, draw_h - surf.height, Pixel{0x10, 0x10, 0x10, 0});
    }

    // 3. Blit surface buffer directly
    disp.blit(surf.x, surf.y, surf.buffer, surf.width, surf.height, draw_w, draw_h);
}

bool drawCursorIfInRect(DisplayDriver& disp, std::int32_t rx, std::int32_t ry, std::uint32_t rw, std::uint32_t rh) {
    if (!g_cursor_visible) return false;
    if (g_cursor_x + 11 >= rx && g_cursor_x < static_cast<std::int32_t>(rx + rw) &&
        g_cursor_y + 16 >= ry && g_cursor_y < static_cast<std::int32_t>(ry + rh)) {
        drawCursor(disp, g_cursor_x, g_cursor_y);
        return true;
    }
    return false;
}

void compose(DisplayDriver& disp) {
    // 1. Render desktop background into BackBuffer
    renderDesktop(disp);

    // 2. Render surfaces into BackBuffer
    bool has_surfaces = false;
    for (std::size_t i = 0; i < MAX_SURFACES; ++i) {
        if (g_surfaces[i].id != 0 && g_surfaces[i].visible && g_surfaces[i].buffer) {
            has_surfaces = true;
            renderSurface(disp, g_surfaces[i]);
        }
    }

    // 3. Render legacy terminal (only if no managed surfaces exist)
    if (!has_surfaces) {
        g_terminal.renderWindowFrame();
    }

    // 4. Render cursor on top
    if (g_cursor_visible) {
        drawCursor(disp, g_cursor_x, g_cursor_y);
    }
}

} // namespace

extern "C" void _start() {
    oz::sys_debug_print("[Display] Minimal Display Server started (Ring 3)!\n\r");

    // Wait for configuration IPC message from init process on Channel 2
    oz::IPCMessage config_msg{};
    if (!oz::sys_channel_recv(CAP_DISPLAY_RECV, &config_msg)) {
        oz::sys_debug_print("[Display] Failed to receive config message!\n\r");
        oz::sys_exit(1);
    }

    std::uint32_t width = static_cast<std::uint32_t>(config_msg.data[0]);
    std::uint32_t height = static_cast<std::uint32_t>(config_msg.data[1]);
    std::size_t size = static_cast<std::size_t>(config_msg.data[2]);

    oz::sys_debug_print("[Display] Received config from init.\n\r");

    if (!g_display.init(width, height, size)) {
        oz::sys_exit(1);
    }

    // Notification-driven event loop
    std::uint32_t notif_cap = oz::sys_notification_create(oz::CAP_UNTYPED);
    if (notif_cap == oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[Display] ERROR: Failed to create Notification!\n\r");
        oz::sys_exit(1);
    }

    static constexpr std::uint64_t BADGE_CONSOLE = 1ULL << 0;
    static constexpr std::uint64_t BADGE_COMMAND = 1ULL << 1;

    if (oz::sys_channel_bind_notification(CAP_DISPLAY_RECV, notif_cap, BADGE_CONSOLE) != 0) {
        oz::sys_debug_print("[Display] ERROR: Failed to bind Console Channel to Notification!\n\r");
        oz::sys_exit(1);
    }
    if (oz::sys_channel_bind_notification(CAP_COMMAND_RECV, notif_cap, BADGE_COMMAND) != 0) {
        oz::sys_debug_print("[Display] ERROR: Failed to bind Command Channel to Notification!\n\r");
        oz::sys_exit(1);
    }

    // Setup interactive terminal window in center of screen
    std::uint32_t win_w = 640;
    std::uint32_t win_h = 380;
    if (win_w + 40 > g_display.width()) win_w = g_display.width() - 40;
    if (win_h + 60 > g_display.height()) win_h = g_display.height() - 60;

    std::uint32_t win_x = (g_display.width() > win_w) ? (g_display.width() - win_w) / 2 : 20;
    std::uint32_t win_y = (g_display.height() > win_h) ? (g_display.height() - win_h) / 2 : 40;

    g_terminal.init(&g_display, win_x, win_y, win_w, win_h);
    compose(g_display);
    g_display.flushAll();
    oz::sys_debug_print("[Display] Notification event loop ready (Surface & Blit engine).\n\r");

    while (true) {
        std::uint64_t badge = 0;
        if (oz::sys_notification_wait(notif_cap, &badge) != 0) {
            continue;
        }

        if (badge & BADGE_CONSOLE) {
            oz::IPCMessage req{};
            if (oz::sys_channel_recv(CAP_DISPLAY_RECV, &req)) {
                alignas(8) char buf[33];
                for (int i = 0; i < 4; ++i) {
                    *reinterpret_cast<std::uint64_t*>(&buf[i * 8]) = req.data[i];
                }
                buf[32] = '\0';
                if (!has_active_surfaces()) {
                    g_terminal.write(buf);
                    g_display.flush(win_x, win_y, win_w, win_h);
                }
            }
        }

        if (badge & BADGE_COMMAND) {
            oz::IPCMessage req{};
            if (!oz::sys_channel_recv(CAP_COMMAND_RECV, &req)) {
                continue;
            }

            auto cmd = static_cast<oz::ipc::DisplayCmd>(req.data[0] & 0xFFFFFFFFULL);
            oz::sys_debug_print("[Display] Handling DisplayCmd ID: ");
            char c_buf[5] = {static_cast<char>('0' + (static_cast<std::uint32_t>(cmd) / 10)),
                             static_cast<char>('0' + (static_cast<std::uint32_t>(cmd) % 10)),
                             '\n', '\r', '\0'};
            oz::sys_debug_print(c_buf);
            oz::IPCMessage resp{};
            std::uint32_t reply_endpoint = req.has_cap ? req.transferred_cap : 0;

                switch (cmd) {
                    case oz::ipc::DisplayCmd::CREATE_SURFACE: {
                        std::uint32_t w = static_cast<std::uint32_t>(req.data[1]);
                        std::uint32_t h = static_cast<std::uint32_t>(req.data[2]);
                        int slot = -1;
                        for (std::size_t i = 0; i < MAX_SURFACES; ++i) {
                            if (g_surfaces[i].id == 0) {
                                slot = static_cast<int>(i);
                                break;
                            }
                        }

                        if (slot >= 0 && w > 0 && h > 0) {
                            std::size_t surf_size = ((w * h * sizeof(Pixel)) + 4095) & ~4095ULL;
                            std::uint64_t num_pages = surf_size / 4096;
                            std::uint32_t frame_cap = oz::sys_frame_alloc(oz::CAP_UNTYPED, num_pages);

                            if (frame_cap != oz::INVALID_CAP_ID) {
                                std::uint64_t virt = SURFACE_BUF_BASE_VIRT + slot * 0x1000000ULL;
                                if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, frame_cap, virt, 0x01, surf_size) == 0) {
                                    g_surfaces[slot].id = slot + 1;
                                    g_surfaces[slot].width = w;
                                    g_surfaces[slot].height = h;
                                    g_surfaces[slot].rect_w = w;
                                    g_surfaces[slot].rect_h = h;
                                    g_surfaces[slot].buffer = reinterpret_cast<Pixel*>(virt);
                                    g_surfaces[slot].frame_cap = frame_cap;
                                    g_surfaces[slot].visible = true;
                                    g_surfaces[slot].size = surf_size;

                                    // Fill buffer with default color
                                    for (std::size_t p = 0; p < w * h; ++p) {
                                        g_surfaces[slot].buffer[p] = Pixel{0x20, 0x20, 0x20, 0};
                                    }

                                    resp.data[0] = 0; // success
                                    resp.data[1] = slot + 1; // surface_id
                                    resp.data[2] = surf_size;
                                    resp.transferred_cap = frame_cap;
                                    resp.has_cap = true;
                                    oz::sys_debug_print("[Display] Created Surface successfully!\n\r");
                                } else {
                                    resp.data[0] = 2; // map failed
                                }
                            } else {
                                resp.data[0] = 3; // frame alloc failed
                            }
                        } else {
                            resp.data[0] = 1; // no free surface slot
                        }
                        break;
                    }
                    case oz::ipc::DisplayCmd::COMMIT: {
                        std::uint32_t surf_id = static_cast<std::uint32_t>(req.data[1]);
                        if (surf_id > 0 && surf_id <= MAX_SURFACES) {
                            const auto& surf = g_surfaces[surf_id - 1];

                            // 1. If surface is hidden/unmapped, drop commit immediately!
                            if (!surf.visible || !surf.buffer) {
                                resp.data[0] = 0;
                                break;
                            }

                            // 2. Client commit: blit surface content (border remains intact)
                            renderSurface(g_display, surf, /* render_border = */ false);

                            std::uint32_t draw_w = (surf.rect_w > 0) ? surf.rect_w : surf.width;
                            std::uint32_t draw_h = (surf.rect_h > 0) ? surf.rect_h : surf.height;

                            bool cursor_drawn = drawCursorIfInRect(g_display, surf.x, surf.y, draw_w, draw_h);
                            g_display.flush(surf.x, surf.y, draw_w, draw_h);
                            if (cursor_drawn) {
                                // Flush full cursor bounding box (16x16) to prevent edge clipping
                                g_display.flush(g_cursor_x, g_cursor_y, 16, 16);
                            }
                        } else {
                            // Full-screen commit (surf_id == 0)
                            compose(g_display);
                            g_display.flushAll();
                        }
                        resp.data[0] = 0;
                        break;
                    }
                    case oz::ipc::DisplayCmd::DESTROY_SURFACE: {
                        std::uint32_t id = static_cast<std::uint32_t>(req.data[1]);
                        if (id > 0 && id <= MAX_SURFACES) {
                            std::size_t idx = id - 1;
                            g_surfaces[idx].id = 0;
                            g_surfaces[idx].visible = false;
                            g_surfaces[idx].buffer = nullptr;
                            g_surfaces[idx].rect_w = 0;
                            g_surfaces[idx].rect_h = 0;
                            g_surfaces[idx].border_width = 0;
                            compose(g_display);
                            g_display.flushAll();
                            resp.data[0] = 0;
                        } else {
                            resp.data[0] = 1;
                        }
                        break;
                    }
                    case oz::ipc::DisplayCmd::SET_SURFACE_RECT: {
                        std::uint32_t id = static_cast<std::uint32_t>(req.data[1]);
                        if (id > 0 && id <= MAX_SURFACES) {
                            std::size_t idx = id - 1;
                            g_surfaces[idx].x = static_cast<std::int32_t>(req.data[2] >> 32);
                            g_surfaces[idx].y = static_cast<std::int32_t>(req.data[2] & 0xFFFFFFFFULL);
                            std::uint32_t rw = static_cast<std::uint32_t>(req.data[3] >> 32);
                            std::uint32_t rh = static_cast<std::uint32_t>(req.data[3] & 0xFFFFFFFFULL);
                            if (rw > 0 && rh > 0) {
                                g_surfaces[idx].rect_w = rw;
                                g_surfaces[idx].rect_h = rh;
                            }
                            resp.data[0] = 0;
                        } else {
                            resp.data[0] = 1;
                        }
                        break;
                    }
                    case oz::ipc::DisplayCmd::SET_SURFACE_VISIBILITY: {
                        std::uint32_t id = static_cast<std::uint32_t>(req.data[1]);
                        if (id > 0 && id <= MAX_SURFACES) {
                            std::size_t idx = id - 1;
                            g_surfaces[idx].visible = (req.data[2] != 0);
                            resp.data[0] = 0;
                        } else {
                            resp.data[0] = 1;
                        }
                        break;
                    }
                    case oz::ipc::DisplayCmd::UPDATE_CURSOR: {
                        std::int32_t old_x = g_cursor_x;
                        std::int32_t old_y = g_cursor_y;
                        g_cursor_x = static_cast<std::int32_t>(req.data[1] >> 32);
                        g_cursor_y = static_cast<std::int32_t>(req.data[1] & 0xFFFFFFFFULL);
                        g_cursor_visible = (req.data[2] != 0);

                        // Recompose BackBuffer and flush only old and new 16x16 cursor bounding boxes
                        compose(g_display);
                        g_display.flush(old_x, old_y, 16, 16);
                        g_display.flush(g_cursor_x, g_cursor_y, 16, 16);
                        resp.data[0] = 0;
                        break;
                    }
                    case oz::ipc::DisplayCmd::GET_DISPLAY_INFO: {
                        resp.data[0] = 0;
                        resp.data[1] = g_display.width();
                        resp.data[2] = g_display.height();
                        break;
                    }
                    case oz::ipc::DisplayCmd::SET_SURFACE_BORDER: {
                        std::uint32_t id = static_cast<std::uint32_t>(req.data[1]);
                        if (id > 0 && id <= MAX_SURFACES) {
                            std::size_t idx = id - 1;
                            g_surfaces[idx].border_width = static_cast<std::uint32_t>(req.data[2]);
                            std::uint32_t col = static_cast<std::uint32_t>(req.data[3]);
                            g_surfaces[idx].border_color = Pixel{
                                static_cast<std::uint8_t>(col & 0xFF),
                                static_cast<std::uint8_t>((col >> 8) & 0xFF),
                                static_cast<std::uint8_t>((col >> 16) & 0xFF),
                                static_cast<std::uint8_t>((col >> 24) & 0xFF)
                            };
                            resp.data[0] = 0;
                        } else {
                            resp.data[0] = 1;
                        }
                        break;
                    }
                    default:
                        resp.data[0] = static_cast<std::uint64_t>(-1);
                        break;
                }

                if (reply_endpoint != 0) {
                    oz::sys_channel_send(reply_endpoint, &resp);
                }
        }
    }
}

