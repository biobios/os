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

        int res = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_FRAMEBUFFER, FB_VADDR, 0x01, size);
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

    void blit(std::int32_t dst_x, std::int32_t dst_y, const Pixel* src, std::uint32_t src_w, std::uint32_t src_h) {
        if (!fb_ || !src) return;

        std::int32_t start_x = (dst_x < 0) ? -dst_x : 0;
        std::int32_t start_y = (dst_y < 0) ? -dst_y : 0;

        std::int32_t end_x = static_cast<std::int32_t>(src_w);
        if (dst_x + end_x > static_cast<std::int32_t>(width_)) {
            end_x = static_cast<std::int32_t>(width_) - dst_x;
        }
        std::int32_t end_y = static_cast<std::int32_t>(src_h);
        if (dst_y + end_y > static_cast<std::int32_t>(height_)) {
            end_y = static_cast<std::int32_t>(height_) - dst_y;
        }

        for (std::int32_t sy = start_y; sy < end_y; ++sy) {
            std::int32_t dy = dst_y + sy;
            if (dy < 0 || dy >= static_cast<std::int32_t>(height_)) continue;
            for (std::int32_t sx = start_x; sx < end_x; ++sx) {
                std::int32_t dx = dst_x + sx;
                if (dx < 0 || dx >= static_cast<std::int32_t>(width_)) continue;
                fb_[dy * width_ + dx] = src[sy * src_w + sx];
            }
        }
    }

    void drawChar(std::uint32_t x, std::uint32_t y, char c, Pixel fg) {
        if (c < 32 || c > 126) return;
        if (x + FONT_WIDTH > width_ || y + FONT_HEIGHT > height_) return;

        for (int r = 0; r < FONT_HEIGHT; ++r) {
            for (int col = 0; col < FONT_WIDTH; ++col) {
                if (font_bitmap[c - 32][r][col]) {
                    fb_[(y + r) * width_ + (x + col)] = fg;
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
        if (lines >= h) {
            fillRect(x, y, w, h, bg);
            return;
        }
        for (std::uint32_t cy = y; cy < y + h - lines; ++cy) {
            for (std::uint32_t cx = x; cx < x + w; ++cx) {
                fb_[cy * width_ + cx] = fb_[(cy + lines) * width_ + cx];
            }
        }
        fillRect(x, y + h - lines, w, lines, bg);
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
static constexpr std::size_t MAX_SURFACES = 8;

struct Surface {
    std::uint32_t id{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::int32_t x{0};
    std::int32_t y{0};
    bool visible{false};
    std::int32_t z_order{0};
    std::uint32_t frame_cap{oz::INVALID_CAP_ID};
    Pixel* buffer{nullptr};
    std::size_t size{0};
};

static Surface g_surfaces[MAX_SURFACES] = {};
static std::int32_t g_cursor_x = 200;
static std::int32_t g_cursor_y = 150;
static bool g_cursor_visible = true;

void compose(DisplayDriver& disp) {
    // 1. Render desktop background
    renderDesktop(disp);

    // 2. Render surfaces
    for (std::size_t i = 0; i < MAX_SURFACES; ++i) {
        if (g_surfaces[i].id != 0 && g_surfaces[i].visible && g_surfaces[i].buffer) {
            disp.blit(g_surfaces[i].x, g_surfaces[i].y, g_surfaces[i].buffer, g_surfaces[i].width, g_surfaces[i].height);
        }
    }

    // 3. Render legacy terminal
    g_terminal.renderWindowFrame();

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
    if (oz::sys_channel_bind_notification(CAP_DISPLAY_RECV, notif_cap, BADGE_CONSOLE) != 0) {
        oz::sys_debug_print("[Display] ERROR: Failed to bind Channel to Notification!\n\r");
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
    oz::sys_debug_print("[Display] Notification event loop ready (Surface & Blit engine).\n\r");

    while (true) {
        std::uint64_t badge = 0;
        if (oz::sys_notification_wait(notif_cap, &badge) != 0) {
            continue;
        }

        if (badge & BADGE_CONSOLE) {
            oz::IPCMessage req{};
            if (!oz::sys_channel_recv(CAP_DISPLAY_RECV, &req)) {
                oz::sys_debug_print("[Display] Client on Channel 2 disconnected.\n\r");
                continue;
            }

            // Check if this is a Display IPC command
            auto cmd = static_cast<oz::ipc::DisplayCmd>(req.data[0] & 0xFFFFFFFFULL);
            bool is_cmd = false;

            if (cmd == oz::ipc::DisplayCmd::CREATE_SURFACE ||
                cmd == oz::ipc::DisplayCmd::COMMIT ||
                cmd == oz::ipc::DisplayCmd::DESTROY_SURFACE ||
                cmd == oz::ipc::DisplayCmd::SET_SURFACE_RECT ||
                cmd == oz::ipc::DisplayCmd::SET_SURFACE_VISIBILITY ||
                cmd == oz::ipc::DisplayCmd::UPDATE_CURSOR ||
                cmd == oz::ipc::DisplayCmd::GET_DISPLAY_INFO) {
                is_cmd = true;
            }

            if (is_cmd) {
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
                        compose(g_display);
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
                            compose(g_display);
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
                            compose(g_display);
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
                            compose(g_display);
                            resp.data[0] = 0;
                        } else {
                            resp.data[0] = 1;
                        }
                        break;
                    }
                    case oz::ipc::DisplayCmd::UPDATE_CURSOR: {
                        g_cursor_x = static_cast<std::int32_t>(req.data[1] >> 32);
                        g_cursor_y = static_cast<std::int32_t>(req.data[1] & 0xFFFFFFFFULL);
                        g_cursor_visible = (req.data[2] != 0);
                        compose(g_display);
                        resp.data[0] = 0;
                        break;
                    }
                    case oz::ipc::DisplayCmd::GET_DISPLAY_INFO: {
                        resp.data[0] = 0;
                        resp.data[1] = g_display.width();
                        resp.data[2] = g_display.height();
                        break;
                    }
                    default:
                        resp.data[0] = static_cast<std::uint64_t>(-1);
                        break;
                }

                if (reply_endpoint != 0) {
                    oz::sys_channel_send(reply_endpoint, &resp);
                }
            } else {
                // Legacy text from console
                alignas(8) char buf[33];
                for (int i = 0; i < 4; ++i) {
                    *reinterpret_cast<std::uint64_t*>(&buf[i * 8]) = req.data[i];
                }
                buf[32] = '\0';
                g_terminal.write(buf);
            }
        }
    }
}

