#include "syscall.hpp"
#include <oz/Font.hpp>
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"
#include "../common/utils/utils.hpp"

OZ_MANIFEST((oz::ProcessManifest{
    .magic = oz::ProcessManifest::MAGIC,
    .version = 1,
    .provides = static_cast<std::uint32_t>(oz::ServiceId::DISPLAY),
    .required = 0,
    .device_needed = oz::DeviceRequirement::FRAMEBUFFER,
    .needs_untyped = true,
    .default_data_dir = {}
}));

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

        // 2. Allocate and map BackBuffer (RAM) via batch frame allocation
        std::uint64_t num_pages = (size + 4095) / 4096;
        std::uint32_t start_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 0, num_pages);
        if (start_frame == oz::INVALID_CAP_ID) {
            oz::sys_debug_print("[Display] ERROR: Failed to allocate BackBuffer frames!\n\r");
            return false;
        }
        res = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, start_frame, BACK_BUF_VADDR, 0x01, size);
        if (res != 0) {
            oz::sys_debug_print("[Display] ERROR: Failed to map BackBuffer!\n\r");
            return false;
        }
        back_buffer_ = reinterpret_cast<Pixel*>(BACK_BUF_VADDR);

        oz::sys_debug_print("[Display] FrameBuffer & BackBuffer mapped successfully.\n\r");
        return true;
    }

    Pixel getPixel(std::int32_t x, std::int32_t y) const {
        if (!back_buffer_ || x < 0 || y < 0 ||
            static_cast<std::uint32_t>(x) >= width_ || static_cast<std::uint32_t>(y) >= height_) {
            return Pixel{0, 0, 0, 0};
        }
        return back_buffer_[static_cast<std::uint32_t>(y) * width_ + static_cast<std::uint32_t>(x)];
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
        std::uint32_t p_val = *reinterpret_cast<const std::uint32_t*>(&color);
        std::uint64_t p_pair = (static_cast<std::uint64_t>(p_val) << 32) | p_val;

        for (std::uint32_t cy = uy; cy < uy + h; ++cy) {
            Pixel* row = &back_buffer_[cy * width_ + ux];
            std::uint32_t cx = 0;
            // 64-bit pair fill
            while (cx + 1 < w) {
                *reinterpret_cast<std::uint64_t*>(&row[cx]) = p_pair;
                cx += 2;
            }
            if (cx < w) {
                row[cx] = color;
            }
        }
    }

    void clear(Pixel color) {
        fillRect(0, 0, width_, height_, color);
    }

    void blit(std::int32_t dst_x, std::int32_t dst_y, const Pixel* src, std::uint32_t src_w, std::uint32_t src_h,
              std::uint32_t max_w = 0, std::uint32_t max_h = 0,
              std::uint32_t sub_x = 0, std::uint32_t sub_y = 0,
              std::uint32_t sub_w = 0, std::uint32_t sub_h = 0) {
        if (!back_buffer_ || !src) return;

        std::uint32_t eff_sub_w = (sub_w > 0) ? sub_w : src_w;
        std::uint32_t eff_sub_h = (sub_h > 0) ? sub_h : src_h;
        if (sub_x >= src_w || sub_y >= src_h) return;
        if (sub_x + eff_sub_w > src_w) eff_sub_w = src_w - sub_x;
        if (sub_y + eff_sub_h > src_h) eff_sub_h = src_h - sub_y;

        std::int32_t render_dst_x = dst_x + static_cast<std::int32_t>(sub_x);
        std::int32_t render_dst_y = dst_y + static_cast<std::int32_t>(sub_y);

        std::int32_t start_x = (render_dst_x < 0) ? -render_dst_x : 0;
        std::int32_t start_y = (render_dst_y < 0) ? -render_dst_y : 0;

        std::int32_t end_x = static_cast<std::int32_t>(eff_sub_w);
        if (max_w > 0 && sub_x + eff_sub_w > max_w) {
            std::int32_t avail = static_cast<std::int32_t>(max_w) - static_cast<std::int32_t>(sub_x);
            if (avail < end_x) end_x = (avail > 0) ? avail : 0;
        }
        if (render_dst_x + end_x > static_cast<std::int32_t>(width_)) {
            end_x = static_cast<std::int32_t>(width_) - render_dst_x;
        }

        std::int32_t end_y = static_cast<std::int32_t>(eff_sub_h);
        if (max_h > 0 && sub_y + eff_sub_h > max_h) {
            std::int32_t avail = static_cast<std::int32_t>(max_h) - static_cast<std::int32_t>(sub_y);
            if (avail < end_y) end_y = (avail > 0) ? avail : 0;
        }
        if (render_dst_y + end_y > static_cast<std::int32_t>(height_)) {
            end_y = static_cast<std::int32_t>(height_) - render_dst_y;
        }

        std::int32_t copy_w = end_x - start_x;
        if (copy_w <= 0) return;

        for (std::int32_t sy = start_y; sy < end_y; ++sy) {
            std::int32_t dy = render_dst_y + sy;
            if (dy < 0 || dy >= static_cast<std::int32_t>(height_)) continue;
            std::size_t dst_offset = static_cast<std::size_t>(dy) * width_ + static_cast<std::size_t>(render_dst_x + start_x);
            std::size_t src_offset = static_cast<std::size_t>(sub_y + sy) * src_w + static_cast<std::size_t>(sub_x + start_x);
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
            std::size_t dst_offset = static_cast<std::size_t>(cy) * width_ + x;
            std::size_t src_offset = static_cast<std::size_t>(cy + lines) * width_ + x;
            __builtin_memcpy(&back_buffer_[dst_offset], &back_buffer_[src_offset], static_cast<std::size_t>(w) * sizeof(Pixel));
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

        // If flushing entire screen width continuously, copy in a single memcpy
        if (x == 0 && w == width_) {
            std::size_t offset = static_cast<std::size_t>(y) * width_;
            Pixel* dst = fb_ + offset;
            const Pixel* src = back_buffer_ + offset;
            __builtin_memcpy(dst, src, static_cast<std::size_t>(h) * width_ * sizeof(Pixel));
            return;
        }

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

static Pixel g_cursor_backup[16][16] = {};
static std::int32_t g_saved_cursor_x = -1;
static std::int32_t g_saved_cursor_y = -1;
static bool g_cursor_has_backup = false;

void restoreCursorBackground(DisplayDriver& disp) {
    if (!g_cursor_has_backup) return;
    for (int r = 0; r < 16; ++r) {
        for (int c = 0; c < 16; ++c) {
            disp.putPixel(g_saved_cursor_x + c, g_saved_cursor_y + r, g_cursor_backup[r][c]);
        }
    }
    g_cursor_has_backup = false;
}

void saveAndDrawCursor(DisplayDriver& disp, std::int32_t x, std::int32_t y) {
    if (!g_cursor_visible) return;
    for (int r = 0; r < 16; ++r) {
        for (int c = 0; c < 16; ++c) {
            g_cursor_backup[r][c] = disp.getPixel(x + c, y + r);
        }
    }
    g_saved_cursor_x = x;
    g_saved_cursor_y = y;
    g_cursor_has_backup = true;

    drawCursor(disp, x, y);
}

void drawSurfaceBorders(DisplayDriver& disp, const Surface& surf) {
    if (surf.id == 0 || !surf.visible || surf.border_width == 0) return;
    std::uint32_t draw_w = (surf.rect_w > 0) ? surf.rect_w : surf.width;
    std::uint32_t draw_h = (surf.rect_h > 0) ? surf.rect_h : surf.height;
    std::int32_t bw = static_cast<std::int32_t>(surf.border_width);

    // Top edge
    disp.fillRect(surf.x - bw, surf.y - bw, draw_w + 2 * bw, bw, surf.border_color);
    disp.flush(surf.x - bw, surf.y - bw, draw_w + 2 * bw, bw);

    // Bottom edge
    disp.fillRect(surf.x - bw, surf.y + static_cast<std::int32_t>(draw_h), draw_w + 2 * bw, bw, surf.border_color);
    disp.flush(surf.x - bw, surf.y + static_cast<std::int32_t>(draw_h), draw_w + 2 * bw, bw);

    // Left edge
    disp.fillRect(surf.x - bw, surf.y, bw, draw_h, surf.border_color);
    disp.flush(surf.x - bw, surf.y, bw, draw_h);

    // Right edge
    disp.fillRect(surf.x + static_cast<std::int32_t>(draw_w), surf.y, bw, draw_h, surf.border_color);
    disp.flush(surf.x + static_cast<std::int32_t>(draw_w), surf.y, bw, draw_h);
}

void renderSurfaceSubRect(DisplayDriver& disp, const Surface& surf,
                          std::uint32_t sub_x, std::uint32_t sub_y,
                          std::uint32_t sub_w, std::uint32_t sub_h) {
    if (surf.id == 0 || !surf.visible || !surf.buffer) return;

    std::uint32_t draw_w = (surf.rect_w > 0) ? surf.rect_w : surf.width;
    std::uint32_t draw_h = (surf.rect_h > 0) ? surf.rect_h : surf.height;

    disp.blit(surf.x, surf.y, surf.buffer, surf.width, surf.height,
              draw_w, draw_h, sub_x, sub_y, sub_w, sub_h);
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

void compose(DisplayDriver& disp) {
    g_cursor_has_backup = false;

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
        saveAndDrawCursor(disp, g_cursor_x, g_cursor_y);
    }
}

} // namespace

static std::uint32_t g_win_x = 0;
static std::uint32_t g_win_y = 0;
static std::uint32_t g_win_w = 0;
static std::uint32_t g_win_h = 0;

static void display_console_worker() {
    oz::ReplyToken ctok = oz::INVALID_REPLY_TOKEN;
    oz::IPCMessage req{};
    oz::IPCMessage cresp{};

    while (true) {
        ctok = oz::sys_reply_recv(ctok, CAP_DISPLAY_RECV, (ctok != oz::INVALID_REPLY_TOKEN) ? &cresp : nullptr, &req);
        if (ctok == oz::INVALID_REPLY_TOKEN) {
            continue;
        }

        alignas(8) char buf[33];
        for (int i = 0; i < 4; ++i) {
            *reinterpret_cast<std::uint64_t*>(&buf[i * 8]) = req.data[i];
        }
        buf[32] = '\0';
        if (!has_active_surfaces()) {
            g_terminal.write(buf);
            g_display.flush(g_win_x, g_win_y, g_win_w, g_win_h);
        }
        cresp = oz::IPCMessage{};
    }
}

extern "C" void oz_main(const oz::ProcessStartupInfo* info) {
    oz::LocalNamespace::init(info);
    oz::sys_debug_print("[Display] Minimal Display Server started (Ring 3)!\n\r");

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t size = 0;

    oz::ReplyToken init_tok = oz::INVALID_REPLY_TOKEN;
    oz::IPCMessage init_ack{};

    std::uint32_t drivman_cap = oz::LocalNamespace::get_cap(oz::CapRole::DRIVMAN);
    if (drivman_cap != 0) {
        bool got_fb = false;
        oz::IPCMessage sess_req{};
        sess_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::INIT_DEVICE_SESSION);
        oz::IPCMessage sess_resp{};
        if (oz::sys_call(drivman_cap, &sess_req, &sess_resp) && sess_resp.data[0] == 0) {
            oz::IPCMessage res_req{};
            res_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::GET_DEVICE_RESOURCE);
            res_req.data[1] = 3; // FRAMEBUFFER
            res_req.data[2] = 0;
            oz::IPCMessage res_resp{};
            bool res_ok = oz::sys_call_recv_cap(drivman_cap, &res_req, &res_resp);
            oz::sys_debug_print("[Display] GET_DEVICE_RESOURCE call=");
            oz::sys_debug_print(res_ok ? "ok" : "fail");
            oz::sys_debug_print(" status=");
            char sbuf[17];
            auto to_hex_local = [](std::uint64_t val, char* out) {
                const char* d = "0123456789ABCDEF";
                for (int i = 15; i >= 0; --i) { out[i] = d[val & 0xF]; val >>= 4; }
                out[16] = '\0';
            };
            to_hex_local(res_resp.data[0], sbuf);
            oz::sys_debug_print(sbuf);
            oz::sys_debug_print(" has_cap=");
            oz::sys_debug_print(res_resp.has_cap ? "1\n\r" : "0\n\r");
            if (res_ok && res_resp.data[0] == 0 && res_resp.has_cap) {
                size = static_cast<std::size_t>(res_resp.data[2]);
                width = static_cast<std::uint32_t>(res_resp.data[3] >> 32);
                height = static_cast<std::uint32_t>(res_resp.data[3] & 0xFFFFFFFFULL);
                oz::sys_cap_move(oz::CAP_FRAMEBUFFER, res_resp.transferred_cap);
                got_fb = true;
                oz::sys_debug_print("[Display] Received FrameBuffer from DrivMan via session.\n\r");
            }
        }

        if (!got_fb) {
            oz::IPCMessage fb_req{};
            fb_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::GET_DEVICE_CAP);
            const char* dname = "fb";
            for (int i = 0; i < 2; ++i) reinterpret_cast<char*>(&fb_req.data[1])[i] = dname[i];
            oz::IPCMessage fb_resp{};
            if (oz::sys_call_recv_cap(drivman_cap, &fb_req, &fb_resp) && fb_resp.data[0] == 0 && fb_resp.has_cap) {
                width = static_cast<std::uint32_t>(fb_resp.data[1] >> 32);
                height = static_cast<std::uint32_t>(fb_resp.data[1] & 0xFFFFFFFFULL);
                size = static_cast<std::size_t>(fb_resp.data[2]);
                oz::sys_cap_move(oz::CAP_FRAMEBUFFER, fb_resp.transferred_cap);
                got_fb = true;
            } else {
                oz::sys_debug_print("[Display] ERROR: Failed to get FrameBuffer from DrivMan!\n\r");
                oz::sys_exit(1);
            }
        }

        oz::IPCMessage online_req{};
        online_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::NOTIFY_DEVICE_ONLINE);
        oz::IPCMessage online_resp{};
        oz::sys_call(drivman_cap, &online_req, &online_resp);

        oz::sys_debug_print("[Display] Received FrameBuffer from DrivMan: w=");
        char hbuf[17];
        auto to_hex = [](std::uint64_t val, char* out) {
            const char* d = "0123456789ABCDEF";
            for (int i = 15; i >= 0; --i) { out[i] = d[val & 0xF]; val >>= 4; }
            out[16] = '\0';
        };
        to_hex(width, hbuf); oz::sys_debug_print(hbuf);
        oz::sys_debug_print(" h=");
        to_hex(height, hbuf); oz::sys_debug_print(hbuf);
        oz::sys_debug_print(" sz=");
        to_hex(size, hbuf); oz::sys_debug_print(hbuf);
        oz::sys_debug_print("\n\r");
    } else {
        // Wait for configuration IPC message from init process on Channel 2
        oz::IPCMessage config_msg{};
        init_tok = oz::sys_reply_recv(oz::INVALID_REPLY_TOKEN, CAP_DISPLAY_RECV, nullptr, &config_msg);
        if (init_tok == oz::INVALID_REPLY_TOKEN) {
            oz::sys_debug_print("[Display] Failed to receive config message!\n\r");
            oz::sys_exit(1);
        }

        width = static_cast<std::uint32_t>(config_msg.data[0]);
        height = static_cast<std::uint32_t>(config_msg.data[1]);
        size = static_cast<std::size_t>(config_msg.data[2]);
        oz::sys_debug_print("[Display] Received config from init.\n\r");
    }

    if (!g_display.init(width, height, size)) {
        oz::sys_exit(1);
    }

    // Setup interactive terminal window in center of screen
    g_win_w = 640;
    g_win_h = 380;
    if (g_win_w + 40 > g_display.width()) g_win_w = g_display.width() - 40;
    if (g_win_h + 60 > g_display.height()) g_win_h = g_display.height() - 60;

    g_win_x = (g_display.width() > g_win_w) ? (g_display.width() - g_win_w) / 2 : 20;
    g_win_y = (g_display.height() > g_win_h) ? (g_display.height() - g_win_h) / 2 : 40;

    g_terminal.init(&g_display, g_win_x, g_win_y, g_win_w, g_win_h);
    compose(g_display);
    g_display.flushAll();

    // Spawn passive worker thread for CAP_DISPLAY_RECV (Console text stream)
    std::uint32_t worker_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t worker_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 2);
    static constexpr std::uint64_t WORKER_STACK_TOP = 0x700010000000ULL;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, worker_stack, WORKER_STACK_TOP - 8192, 0x01, 8192);
    oz::sys_tcb_configure(worker_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(display_console_worker), WORKER_STACK_TOP - 8);
    oz::sys_tcb_resume(worker_tcb);

    // Initial hand-off to let worker reach sys_reply_recv(token=0) and unbind
    std::uint32_t worker_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_tcb_bind_baton(worker_tcb, worker_baton);
    oz::sys_baton_inject(worker_baton, 10000);
    oz::SchedResult w_res = oz::sys_baton_switch(worker_baton);
    if (w_res.reason == oz::SchedReason::ORPHANED || w_res.reason == oz::SchedReason::BLOCKED) {
        oz::sys_debug_print("[Display] Console worker initialized and passive.\n\r");
    }
    oz::sys_cap_delete(worker_baton);

    oz::sys_debug_print("[Display] Main command loop ready (Surface & Blit engine).\n\r");

    oz::ReplyToken cmd_tok = init_tok;
    oz::IPCMessage req{};
    oz::IPCMessage resp = init_ack;

    while (true) {
        cmd_tok = oz::sys_reply_recv(cmd_tok, CAP_COMMAND_RECV, (cmd_tok != oz::INVALID_REPLY_TOKEN) ? &resp : nullptr, &req);
        if (cmd_tok == oz::INVALID_REPLY_TOKEN) {
            continue;
        }

        auto cmd = static_cast<oz::ipc::DisplayCmd>(req.data[0] & 0xFFFFFFFFULL);
        resp = oz::IPCMessage{};

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
                            std::uint32_t frame_cap = oz::sys_alloc_shared_pt(oz::CAP_UNTYPED, num_pages, 0x01);

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

                                    // Mint an unmapped copy to transfer to the client (1-slot-1-mapping compliant)
                                    std::uint32_t client_frame_cap = oz::sys_cap_mint(frame_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
                                    if (client_frame_cap != oz::INVALID_CAP_ID) {
                                        resp.data[0] = 0; // success
                                        resp.data[1] = slot + 1; // surface_id
                                        resp.data[2] = surf_size;
                                        resp.transferred_cap = client_frame_cap;
                                        resp.has_cap = true;
                                        oz::sys_debug_print("[Display] Created Surface successfully!\n\r");
                                    } else {
                                        resp.data[0] = 3;
                                    }
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

                            std::uint32_t draw_w = (surf.rect_w > 0) ? surf.rect_w : surf.width;
                            std::uint32_t draw_h = (surf.rect_h > 0) ? surf.rect_h : surf.height;

                            std::uint32_t dirty_x = static_cast<std::uint32_t>(req.data[2] >> 32);
                            std::uint32_t dirty_y = static_cast<std::uint32_t>(req.data[2] & 0xFFFFFFFFULL);
                            std::uint32_t dirty_w = static_cast<std::uint32_t>(req.data[3] >> 32);
                            std::uint32_t dirty_h = static_cast<std::uint32_t>(req.data[3] & 0xFFFFFFFFULL);

                            if (dirty_w == 0 || dirty_h == 0) {
                                dirty_x = 0;
                                dirty_y = 0;
                                dirty_w = draw_w;
                                dirty_h = draw_h;
                            }

                            if (dirty_x >= draw_w || dirty_y >= draw_h) {
                                resp.data[0] = 0;
                                break;
                            }
                            if (dirty_x + dirty_w > draw_w) dirty_w = draw_w - dirty_x;
                            if (dirty_y + dirty_h > draw_h) dirty_h = draw_h - dirty_y;

                            std::int32_t flush_x = surf.x + static_cast<std::int32_t>(dirty_x);
                            std::int32_t flush_y = surf.y + static_cast<std::int32_t>(dirty_y);

                            // Check if mouse cursor intersects with dirty rect
                            bool cursor_overlap = false;
                            if (g_cursor_visible && g_cursor_has_backup) {
                                if (g_cursor_x + 16 > flush_x && g_cursor_x < flush_x + static_cast<std::int32_t>(dirty_w) &&
                                    g_cursor_y + 16 > flush_y && g_cursor_y < flush_y + static_cast<std::int32_t>(dirty_h)) {
                                    cursor_overlap = true;
                                    restoreCursorBackground(g_display);
                                }
                            }

                            // 2. Client commit: blit only dirty rect into BackBuffer
                            renderSurfaceSubRect(g_display, surf, dirty_x, dirty_y, dirty_w, dirty_h);

                            if (cursor_overlap) {
                                saveAndDrawCursor(g_display, g_cursor_x, g_cursor_y);
                            }

                            // 3. Flush only dirty rect to physical VRAM
                            g_display.flush(flush_x, flush_y, dirty_w, dirty_h);
                            if (cursor_overlap) {
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
                            if (g_surfaces[idx].frame_cap != 0) {
                                oz::sys_cap_delete(g_surfaces[idx].frame_cap);
                                g_surfaces[idx].frame_cap = 0;
                            }
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
                        std::int32_t new_x = static_cast<std::int32_t>(req.data[1] >> 32);
                        std::int32_t new_y = static_cast<std::int32_t>(req.data[1] & 0xFFFFFFFFULL);
                        bool new_vis = (req.data[2] != 0);

                        std::int32_t old_x = g_saved_cursor_x;
                        std::int32_t old_y = g_saved_cursor_y;

                        // 1. Restore previous cursor background in BackBuffer (NO compose()!!)
                        restoreCursorBackground(g_display);

                        g_cursor_x = new_x;
                        g_cursor_y = new_y;
                        g_cursor_visible = new_vis;

                        // 2. Save new background and draw cursor in BackBuffer
                        if (g_cursor_visible) {
                            saveAndDrawCursor(g_display, g_cursor_x, g_cursor_y);
                        }

                        // 3. High-speed flush of only old and new 16x16 cursor bounding boxes
                        if (old_x >= 0 && old_y >= 0) {
                            g_display.flush(old_x, old_y, 16, 16);
                        }
                        if (g_cursor_visible) {
                            g_display.flush(g_cursor_x, g_cursor_y, 16, 16);
                        }
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
                            // Fast path: if visible and borders defined, update borders immediately
                            if (g_surfaces[idx].visible && g_surfaces[idx].border_width > 0) {
                                drawSurfaceBorders(g_display, g_surfaces[idx]);
                            }
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
    }
}

