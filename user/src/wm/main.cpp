#include "syscall.hpp"
#include "Layout.hpp"
#include "WindowManager.hpp"
#include "../display/Font.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/utils/utils.hpp"

namespace {

struct Pixel {
    std::uint8_t b;
    std::uint8_t g;
    std::uint8_t r;
    std::uint8_t a;
};

// Cap slots for Window Manager
static constexpr std::uint32_t CAP_CONSOLE_SEND = 1;
static constexpr std::uint32_t CAP_DISPLAY_SEND = 2;
static constexpr std::uint32_t CAP_DISPLAY_RECV = 3;
static constexpr std::uint32_t CAP_WM_RECV      = 4;

static constexpr std::uint64_t BAR_SURF_VADDR = 0x750000000000ULL;
static constexpr std::uint64_t CLIENT_SURF_BASE_VADDR = 0x760000000000ULL;

static oz::wm::WindowManager g_wm;
static std::uint32_t g_bar_surf_id = 0;
static Pixel* g_bar_buffer = nullptr;
static std::uint32_t g_screen_width = 800;
static std::uint32_t g_screen_height = 600;

// Helper: send IPC to Display Server and wait for response
bool display_send_sync(oz::IPCMessage* req, oz::IPCMessage* resp) {
    std::uint32_t reply_send = 0;
    std::uint32_t reply_recv = 0;
    if (!oz::sys_channel_create(oz::CAP_UNTYPED, &reply_send, &reply_recv)) {
        oz::sys_debug_print("[WM] display_send_sync: sys_channel_create FAILED!\n\r");
        return false;
    }
    req->transferred_cap = reply_send;
    req->has_cap = true;
    if (!oz::sys_channel_send(CAP_DISPLAY_SEND, req)) {
        oz::sys_debug_print("[WM] display_send_sync: sys_channel_send FAILED!\n\r");
        oz::sys_cap_delete(reply_recv);
        return false;
    }
    bool ok = oz::sys_channel_recv(reply_recv, resp);
    if (!ok) {
        oz::sys_debug_print("[WM] display_send_sync: sys_channel_recv FAILED!\n\r");
    }
    oz::sys_cap_delete(reply_recv);
    return ok;
}

// Draw a single character in the status bar buffer
void bar_draw_char(std::uint32_t x, std::uint32_t y, char c, Pixel fg, Pixel bg) {
    if (c < 32 || c > 126) return;
    if (x + FONT_WIDTH > g_screen_width || y + FONT_HEIGHT > oz::wm::STATUS_BAR_HEIGHT) return;

    for (int r = 0; r < FONT_HEIGHT; ++r) {
        for (int col = 0; col < FONT_WIDTH; ++col) {
            Pixel p = font_bitmap[c - 32][r][col] ? fg : bg;
            g_bar_buffer[(y + r) * g_screen_width + (x + col)] = p;
        }
    }
}

// Draw a string in the status bar buffer with 1px letter spacing
std::uint32_t bar_draw_string(std::uint32_t x, std::uint32_t y, const char* str, Pixel fg, Pixel bg) {
    while (*str) {
        bar_draw_char(x, y, *str, fg, bg);
        // Clear 1px column after character to avoid glyph touching
        if (x + FONT_WIDTH < g_screen_width) {
            for (int r = 0; r < FONT_HEIGHT; ++r) {
                if (y + r < oz::wm::STATUS_BAR_HEIGHT) {
                    g_bar_buffer[(y + r) * g_screen_width + (x + FONT_WIDTH)] = bg;
                }
            }
        }
        x += FONT_WIDTH + 1;
        str++;
    }
    return x;
}

// Render Top Status Bar
void render_status_bar() {
    if (!g_bar_buffer) return;

    Pixel bg_bar{0x2b, 0x24, 0x1f, 0};      // Dark brown/grey
    Pixel fg_active{0xff, 0xff, 0x00, 0};   // Cyan (b=0xff, g=0xff, r=0x00)
    Pixel fg_inactive{0x88, 0x88, 0x88, 0}; // Dim grey
    Pixel fg_has_win{0xff, 0xff, 0xff, 0};  // White
    Pixel fg_text{0xee, 0xee, 0xee, 0};     // Light grey

    // Clear status bar background
    for (std::uint32_t y = 0; y < oz::wm::STATUS_BAR_HEIGHT; ++y) {
        for (std::uint32_t x = 0; x < g_screen_width; ++x) {
            g_bar_buffer[y * g_screen_width + x] = bg_bar;
        }
    }

    std::uint32_t cx = 8;
    std::uint32_t cy = 4;

    // 1. Workspaces [1]  2  3  ...
    for (std::uint8_t i = 0; i < oz::wm::MAX_WORKSPACES; ++i) {
        char ws_label[5] = " 1 ";
        ws_label[1] = '1' + i;

        if (i == g_wm.active_workspace()) {
            ws_label[0] = '[';
            ws_label[2] = ']';
            cx = bar_draw_string(cx, cy, ws_label, fg_active, bg_bar);
        } else if (g_wm.workspace_has_windows(i)) {
            cx = bar_draw_string(cx, cy, ws_label, fg_has_win, bg_bar);
        } else {
            cx = bar_draw_string(cx, cy, ws_label, fg_inactive, bg_bar);
        }
        cx += 2;
    }

    // 2. Separator
    cx = bar_draw_string(cx + 8, cy, "|", fg_inactive, bg_bar);
    cx += 8;

    // 3. Current Mode
    if (g_wm.mode() == oz::wm::WMMode::RESIZE) {
        cx = bar_draw_string(cx, cy, "MODE: RESIZE", Pixel{0x00, 0x88, 0xff, 0}, bg_bar);
    } else if (g_wm.mode() == oz::wm::WMMode::LAUNCHER) {
        cx = bar_draw_string(cx, cy, "MODE: LAUNCHER", Pixel{0x00, 0xff, 0x88, 0}, bg_bar);
    } else {
        cx = bar_draw_string(cx, cy, "NORMAL", fg_text, bg_bar);
    }

    // 4. Separator
    cx = bar_draw_string(cx + 8, cy, "|", fg_inactive, bg_bar);
    cx += 8;

    // 5. Focused Window Title
    auto* focused_win = g_wm.get_focused_window();
    if (focused_win) {
        cx = bar_draw_string(cx, cy, "Win: ", fg_inactive, bg_bar);
        const char* title = (focused_win->title[0] != '\0') ? focused_win->title : "Terminal";
        cx = bar_draw_string(cx, cy, title, fg_active, bg_bar);
    } else {
        cx = bar_draw_string(cx, cy, "No Window", fg_inactive, bg_bar);
    }

    // 6. Minimized count (Scratchpad)
    if (g_wm.scratchpad_count() > 0) {
        cx = bar_draw_string(cx + 8, cy, "|", fg_inactive, bg_bar);
        cx += 8;
        char min_str[16] = "Min: [0]";
        min_str[6] = '0' + static_cast<char>(g_wm.scratchpad_count() % 10);
        bar_draw_string(cx, cy, min_str, Pixel{0xff, 0xaa, 0x00, 0}, bg_bar);
    }

    // 7. Right-aligned OS Title
    const char* os_title = "OZ Tiling WM";
    std::uint32_t title_len = 12;
    std::uint32_t right_x = g_screen_width - (title_len * (FONT_WIDTH + 1)) - 12;
    bar_draw_string(right_x, cy, os_title, fg_inactive, bg_bar);

    // Commit bar surface to display
    oz::IPCMessage commit_msg{};
    commit_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::COMMIT);
    commit_msg.data[1] = g_bar_surf_id;
    oz::sys_channel_send(CAP_DISPLAY_SEND, &commit_msg);
}

// Recursive helper to update surface positions and borders for active workspace
void sync_node_to_display(oz::wm::LayoutNode* node, std::uint32_t focused_id) {
    if (!node) return;

    if (node->is_leaf()) {
        auto* win = g_wm.get_window(node->id);
        if (!win || win->surface_id == 0) return;

        oz::wm::Rect r = node->rect;
        std::uint32_t bw = oz::wm::BORDER_THICKNESS;

        // Position surface inside borders
        std::int32_t surf_x = r.x + static_cast<std::int32_t>(bw);
        std::int32_t surf_y = r.y + static_cast<std::int32_t>(bw);
        std::uint32_t surf_w = (r.w > 2 * bw) ? r.w - 2 * bw : r.w;
        std::uint32_t surf_h = (r.h > 2 * bw) ? r.h - 2 * bw : r.h;

        // Set Rect in Display
        oz::IPCMessage pos_msg{};
        pos_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::SET_SURFACE_RECT);
        pos_msg.data[1] = win->surface_id;
        pos_msg.data[2] = (static_cast<std::uint64_t>(surf_x) << 32) | (static_cast<std::uint64_t>(surf_y) & 0xFFFFFFFFULL);
        pos_msg.data[3] = (static_cast<std::uint64_t>(surf_w) << 32) | (static_cast<std::uint64_t>(surf_h) & 0xFFFFFFFFULL);
        oz::sys_channel_send(CAP_DISPLAY_SEND, &pos_msg);

        // Set Border: Focused window = Cyan, Inactive = Dim Slate (0x444444)
        bool is_focused = (win->id == focused_id);
        std::uint32_t border_color = is_focused ? 0x0000FFFF : 0x00444444; // Cyan (b=0xff, g=0xff, r=0x00)

        oz::IPCMessage border_msg{};
        border_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::SET_SURFACE_BORDER);
        border_msg.data[1] = win->surface_id;
        border_msg.data[2] = bw;
        border_msg.data[3] = border_color;
        oz::sys_channel_send(CAP_DISPLAY_SEND, &border_msg);

        // Ensure visibility is ON
        oz::IPCMessage vis_msg{};
        vis_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::SET_SURFACE_VISIBILITY);
        vis_msg.data[1] = win->surface_id;
        vis_msg.data[2] = 1;
        oz::sys_channel_send(CAP_DISPLAY_SEND, &vis_msg);

        return;
    }

    sync_node_to_display(node->first, focused_id);
    sync_node_to_display(node->second, focused_id);
}

// Sync entire WindowManager state to Display server
void sync_state_to_display() {
    oz::sys_debug_print("[WM] sync_state_to_display: start\n\r");
    std::uint8_t cur_ws = g_wm.active_workspace();
    std::uint32_t focused_id = g_wm.focused_window_id();

    // 1. Hide all surfaces not in current workspace, or minimized
    for (std::size_t i = 0; i < oz::wm::MAX_WINDOWS; ++i) {
        auto* win = g_wm.get_window(static_cast<std::uint32_t>(i + 1));
        if (win && win->surface_id != 0) {
            if (win->workspace_idx != cur_ws || win->is_minimized) {
                oz::IPCMessage hide_msg{};
                hide_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::SET_SURFACE_VISIBILITY);
                hide_msg.data[1] = win->surface_id;
                hide_msg.data[2] = 0;
                oz::sys_channel_send(CAP_DISPLAY_SEND, &hide_msg);
            }
        }
    }

    // 2. Position and show active workspace windows
    sync_node_to_display(g_wm.current_layout().root(), focused_id);

    // 3. Redraw status bar
    render_status_bar();

    // 4. Commit all surface composition
    oz::IPCMessage commit_msg{};
    commit_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::COMMIT);
    commit_msg.data[1] = 0; // Commit all
    oz::sys_channel_send(CAP_DISPLAY_SEND, &commit_msg);
    oz::sys_debug_print("[WM] sync_state_to_display: done\n\r");
}

// Handle Keyboard Input from xHCI
void handle_key_input(char ascii, std::uint8_t scancode, std::uint32_t modifiers) {
    bool mod_down = (modifiers & (static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER) |
                                  static_cast<std::uint32_t>(oz::ipc::WMModifier::ALT))) != 0;
    bool shift_down = (modifiers & static_cast<std::uint32_t>(oz::ipc::WMModifier::SHIFT)) != 0;

    // Global WM Hotkeys (Mod + ...)
    if (mod_down) {
        // Workspace switching: Super + 1..9
        if (ascii >= '1' && ascii <= '9') {
            std::uint8_t target_ws = static_cast<std::uint8_t>(ascii - '1');
            if (shift_down) {
                // Move focused window to target workspace
                oz::sys_debug_print("[WM] Hotkey: Move focused window to Workspace ");
                char num[4] = {static_cast<char>('1' + target_ws), '\n', '\r', '\0'};
                oz::sys_debug_print(num);
                g_wm.move_focused_to_workspace(target_ws);
            } else {
                // Switch active workspace
                oz::sys_debug_print("[WM] Hotkey: Switch Workspace to ");
                char num[4] = {static_cast<char>('1' + target_ws), '\n', '\r', '\0'};
                oz::sys_debug_print(num);
                g_wm.switch_workspace(target_ws);
            }
            sync_state_to_display();
            return;
        }

        // Focus navigation: Super + h/j/k/l
        if (ascii == 'h') {
            oz::sys_debug_print("[WM] Hotkey: Focus LEFT\n\r");
            g_wm.move_focus(oz::wm::Direction::LEFT);
            sync_state_to_display();
            return;
        } else if (ascii == 'l') {
            oz::sys_debug_print("[WM] Hotkey: Focus RIGHT\n\r");
            g_wm.move_focus(oz::wm::Direction::RIGHT);
            sync_state_to_display();
            return;
        } else if (ascii == 'k') {
            oz::sys_debug_print("[WM] Hotkey: Focus UP\n\r");
            g_wm.move_focus(oz::wm::Direction::UP);
            sync_state_to_display();
            return;
        } else if (ascii == 'j') {
            oz::sys_debug_print("[WM] Hotkey: Focus DOWN\n\r");
            g_wm.move_focus(oz::wm::Direction::DOWN);
            sync_state_to_display();
            return;
        }

        // Minimize / Scratchpad: Super + m
        if (ascii == 'm') {
            if (shift_down) {
                oz::sys_debug_print("[WM] Hotkey: Restore Minimized (Scratchpad)\n\r");
                g_wm.restore_minimized();
            } else {
                oz::sys_debug_print("[WM] Hotkey: Minimize Focused (Scratchpad)\n\r");
                g_wm.minimize_focused();
            }
            sync_state_to_display();
            return;
        }

        // Close window: Super + Shift + q
        if ((ascii == 'q' || ascii == 'Q') && shift_down) {
            std::uint32_t focused_id = g_wm.focused_window_id();
            if (focused_id != 0) {
                oz::sys_debug_print("[WM] Hotkey: Close focused window\n\r");
                auto* win = g_wm.get_window(focused_id);
                if (win && win->surface_id != 0) {
                    oz::IPCMessage ds_msg{};
                    ds_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::DESTROY_SURFACE);
                    ds_msg.data[1] = win->surface_id;
                    oz::sys_channel_send(CAP_DISPLAY_SEND, &ds_msg);
                }
                g_wm.destroy_window(focused_id);
                sync_state_to_display();
            }
            return;
        }

        // Resize Mode: Super + r
        if (ascii == 'r') {
            if (g_wm.mode() == oz::wm::WMMode::RESIZE) {
                oz::sys_debug_print("[WM] Hotkey: Exit RESIZE mode\n\r");
                g_wm.set_mode(oz::wm::WMMode::NORMAL);
            } else {
                oz::sys_debug_print("[WM] Hotkey: Enter RESIZE mode\n\r");
                g_wm.set_mode(oz::wm::WMMode::RESIZE);
            }
            render_status_bar();
            return;
        }
    }

    // Modal behavior: Resize Mode (without Super)
    if (g_wm.mode() == oz::wm::WMMode::RESIZE) {
        if (ascii == 'h' || ascii == 'k') {
            oz::sys_debug_print("[WM] Resize Mode: Shrink ratio (-0.05)\n\r");
            g_wm.resize_focused(-0.05f); // Shrink
            sync_state_to_display();
            return;
        } else if (ascii == 'l' || ascii == 'j') {
            oz::sys_debug_print("[WM] Resize Mode: Grow ratio (+0.05)\n\r");
            g_wm.resize_focused(0.05f); // Grow
            sync_state_to_display();
            return;
        } else if (ascii == '\r' || ascii == 27) { // Enter or Escape
            oz::sys_debug_print("[WM] Resize Mode: Exit to NORMAL mode\n\r");
            g_wm.set_mode(oz::wm::WMMode::NORMAL);
            render_status_bar();
            return;
        }
    }

    // Pass-through: send key event directly to focused window's input channel
    auto* win = g_wm.get_focused_window();
    if (win && win->input_channel_send != 0) {
        oz::IPCMessage event_msg{};
        event_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMEventType::KEY_DOWN);
        event_msg.data[1] = static_cast<std::uint64_t>(ascii);
        event_msg.data[2] = static_cast<std::uint64_t>(scancode);
        event_msg.data[3] = static_cast<std::uint64_t>(modifiers);
        oz::sys_channel_send(win->input_channel_send, &event_msg);
    }
}

} // namespace

extern "C" void _start() {
    oz::sys_debug_print("[WM] Tiling Window Manager Server started (Ring 3)!\n\r");

    std::uint32_t t_ep0 = 0, t_ep1 = 0;
    if (oz::sys_channel_create(oz::CAP_UNTYPED, &t_ep0, &t_ep1)) {
        oz::sys_debug_print("[WM] CAP_UNTYPED is VALID!\n\r");
        oz::sys_cap_delete(t_ep0);
        oz::sys_cap_delete(t_ep1);
    } else {
        oz::sys_debug_print("[WM] ERROR: CAP_UNTYPED is INVALID or EXHAUSTED!\n\r");
    }

    // 1. Query Display Server for Screen Dimensions
    oz::IPCMessage req_info{};
    req_info.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::GET_DISPLAY_INFO);
    oz::IPCMessage resp_info{};

    if (display_send_sync(&req_info, &resp_info) && resp_info.data[0] == 0) {
        g_screen_width = static_cast<std::uint32_t>(resp_info.data[1]);
        g_screen_height = static_cast<std::uint32_t>(resp_info.data[2]);
        oz::sys_debug_print("[WM] Connected to Display Server. Screen resolution: ");
        char dim_buf[16] = {
            static_cast<char>('0' + (g_screen_width / 1000 % 10)),
            static_cast<char>('0' + (g_screen_width / 100 % 10)),
            static_cast<char>('0' + (g_screen_width / 10 % 10)),
            static_cast<char>('0' + (g_screen_width % 10)),
            'x',
            static_cast<char>('0' + (g_screen_height / 1000 % 10)),
            static_cast<char>('0' + (g_screen_height / 100 % 10)),
            static_cast<char>('0' + (g_screen_height / 10 % 10)),
            static_cast<char>('0' + (g_screen_height % 10)),
            '\n', '\r', '\0'
        };
        oz::sys_debug_print(dim_buf);
    } else {
        oz::sys_debug_print("[WM] WARNING: Failed to query Display Server! Using default 800x600.\n\r");
    }

    // 2. Initialize Window Manager Layout and Workspaces
    g_wm.init(g_screen_width, g_screen_height);

    // 3. Allocate Top Status Bar Surface in Display Server
    oz::IPCMessage req_bar{};
    req_bar.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::CREATE_SURFACE);
    req_bar.data[1] = g_screen_width;
    req_bar.data[2] = oz::wm::STATUS_BAR_HEIGHT;
    oz::IPCMessage resp_bar{};

    if (display_send_sync(&req_bar, &resp_bar) && resp_bar.data[0] == 0 && resp_bar.has_cap) {
        g_bar_surf_id = static_cast<std::uint32_t>(resp_bar.data[1]);
        std::uint32_t bar_frame_cap = resp_bar.transferred_cap;

        std::size_t bar_size = (g_screen_width * oz::wm::STATUS_BAR_HEIGHT * sizeof(Pixel) + 4095) & ~4095ULL;
        if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, bar_frame_cap, BAR_SURF_VADDR, 0x01, bar_size) == 0) {
            g_bar_buffer = reinterpret_cast<Pixel*>(BAR_SURF_VADDR);
            oz::sys_debug_print("[WM] Status bar surface mapped successfully.\n\r");

            // Position status bar at top (y=0) and make visible
            oz::IPCMessage pos_bar{};
            pos_bar.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::SET_SURFACE_RECT);
            pos_bar.data[1] = g_bar_surf_id;
            pos_bar.data[2] = 0; // (x=0, y=0)
            pos_bar.data[3] = (static_cast<std::uint64_t>(g_screen_width) << 32) | oz::wm::STATUS_BAR_HEIGHT;
            oz::sys_channel_send(CAP_DISPLAY_SEND, &pos_bar);

            oz::IPCMessage vis_bar{};
            vis_bar.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::SET_SURFACE_VISIBILITY);
            vis_bar.data[1] = g_bar_surf_id;
            vis_bar.data[2] = 1;
            oz::sys_channel_send(CAP_DISPLAY_SEND, &vis_bar);

            render_status_bar();
        } else {
            oz::sys_debug_print("[WM] ERROR: Failed to map status bar frame buffer!\n\r");
        }
    } else {
        oz::sys_debug_print("[WM] ERROR: Failed to create status bar surface!\n\r");
    }

    // 4. Create Notification Event Loop
    std::uint32_t notif_cap = oz::sys_notification_create(oz::CAP_UNTYPED);
    if (notif_cap == oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[WM] ERROR: Failed to create Notification!\n\r");
        oz::sys_exit(1);
    }

    static constexpr std::uint64_t BADGE_WM = 1ULL << 0;
    if (oz::sys_channel_bind_notification(CAP_WM_RECV, notif_cap, BADGE_WM) != 0) {
        oz::sys_debug_print("[WM] ERROR: Failed to bind WM Channel to Notification!\n\r");
        oz::sys_exit(1);
    }

    oz::sys_debug_print("[WM] Event loop ready. Tiling Window Manager operational.\n\r");

    // 5. Main Event Loop
    while (true) {
        std::uint64_t badge = 0;
        if (oz::sys_notification_wait(notif_cap, &badge) != 0) {
            continue;
        }

        if (badge & BADGE_WM) {
            oz::sys_debug_print("[WM] Notification received BADGE_WM!\n\r");
            oz::IPCMessage req{};
            if (!oz::sys_channel_recv(CAP_WM_RECV, &req)) {
                oz::sys_debug_print("[WM] sys_channel_recv on CAP_WM_RECV failed!\n\r");
                continue;
            }
            oz::sys_debug_print("[WM] Received IPC request successfully!\n\r");

            // Distinguish between Keyboard Event from xHCI vs WM IPC Command
            if (req.data[0] == static_cast<std::uint64_t>(oz::ipc::WMEventType::KEY_DOWN)) {
                char ascii = static_cast<char>(req.data[1]);
                std::uint8_t scancode = static_cast<std::uint8_t>(req.data[2]);
                std::uint32_t modifiers = static_cast<std::uint32_t>(req.data[3]);
                handle_key_input(ascii, scancode, modifiers);
                continue;
            }

            // Otherwise, it's a WM IPC Command
            auto cmd = static_cast<oz::ipc::WMCmd>(req.data[0] & 0xFFFFFFFFULL);
            oz::sys_debug_print("[WM] Processing command ID: ");
            char c_buf[5] = {static_cast<char>('0' + (static_cast<std::uint32_t>(cmd) / 10)),
                             static_cast<char>('0' + (static_cast<std::uint32_t>(cmd) % 10)),
                             '\n', '\r', '\0'};
            oz::sys_debug_print(c_buf);
            oz::IPCMessage resp{};
            std::uint32_t reply_endpoint = (req.has_cap && cmd != oz::ipc::WMCmd::BIND_INPUT_CHANNEL) ? req.transferred_cap : 0;

            switch (cmd) {
                case oz::ipc::WMCmd::CREATE_WINDOW: {
                    oz::sys_debug_print("[WM] Handling CREATE_WINDOW command...\n\r");
                    // Client requests window creation: data[1] = width, data[2] = height
                    std::uint32_t req_w = static_cast<std::uint32_t>(req.data[1]);
                    std::uint32_t req_h = static_cast<std::uint32_t>(req.data[2]);
                    if (req_w == 0) req_w = 400;
                    if (req_h == 0) req_h = 300;

                    // 1. Create Surface in Display Server
                    oz::IPCMessage surf_req{};
                    surf_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::CREATE_SURFACE);
                    surf_req.data[1] = req_w;
                    surf_req.data[2] = req_h;
                    oz::IPCMessage surf_resp{};

                    bool ds_ok = display_send_sync(&surf_req, &surf_resp);
                    if (ds_ok && surf_resp.data[0] == 0 && surf_resp.has_cap) {
                        oz::sys_debug_print("[WM] Surface created for window!\n\r");
                        std::uint32_t surf_id = static_cast<std::uint32_t>(surf_resp.data[1]);
                        std::uint32_t surf_frame_cap = surf_resp.transferred_cap;

                        // 2. Create Input Channel for window
                        std::uint32_t input_send = 0;
                        std::uint32_t input_recv = 0;
                        oz::sys_channel_create(oz::CAP_UNTYPED, &input_send, &input_recv);

                        // 3. Register in Window Manager
                        auto* win = g_wm.alloc_window(surf_id, input_send);
                        if (win) {
                            g_wm.place_window(win->id);
                            oz::sys_debug_print("[WM] Window placed in tiling layout! Syncing to display...\n\r");
                            sync_state_to_display();

                            resp.data[0] = 0; // Success
                            resp.data[1] = win->id;
                            resp.data[2] = surf_id;
                            resp.transferred_cap = surf_frame_cap; // Grant FrameCap to client
                            resp.has_cap = true;
                        } else {
                            resp.data[0] = 2; // Window table full
                        }
                    } else {
                        oz::sys_debug_print("[WM] ERROR: Surface creation failed in display!\n\r");
                        resp.data[0] = 1; // Surface creation failed
                    }
                    break;
                }
                case oz::ipc::WMCmd::DESTROY_WINDOW: {
                    std::uint32_t win_id = static_cast<std::uint32_t>(req.data[1]);
                    auto* win = g_wm.get_window(win_id);
                    if (win) {
                        // Destroy surface in display
                        oz::IPCMessage ds_msg{};
                        ds_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::DESTROY_SURFACE);
                        ds_msg.data[1] = win->surface_id;
                        oz::sys_channel_send(CAP_DISPLAY_SEND, &ds_msg);

                        g_wm.destroy_window(win_id);
                        sync_state_to_display();
                        resp.data[0] = 0;
                    } else {
                        resp.data[0] = 1;
                    }
                    break;
                }
                case oz::ipc::WMCmd::SET_TITLE: {
                    std::uint32_t win_id = static_cast<std::uint32_t>(req.data[1]);
                    auto* win = g_wm.get_window(win_id);
                    if (win) {
                        char* title_dst = win->title;
                        const char* src = reinterpret_cast<const char*>(&req.data[2]);
                        for (int i = 0; i < 16 && src[i] != '\0'; ++i) {
                            title_dst[i] = src[i];
                            title_dst[i + 1] = '\0';
                        }
                        render_status_bar();
                        resp.data[0] = 0;
                    } else {
                        resp.data[0] = 1;
                    }
                    break;
                }
                case oz::ipc::WMCmd::FOCUS_WINDOW: {
                    std::uint32_t win_id = static_cast<std::uint32_t>(req.data[1]);
                    auto* win = g_wm.get_window(win_id);
                    if (win) {
                        g_wm.switch_workspace(win->workspace_idx);
                        sync_state_to_display();
                        resp.data[0] = 0;
                    } else {
                        resp.data[0] = 1;
                    }
                    break;
                }
                case oz::ipc::WMCmd::BIND_INPUT_CHANNEL: {
                    std::uint32_t win_id = static_cast<std::uint32_t>(req.data[1]);
                    auto* win = g_wm.get_window(win_id);
                    if (win && req.has_cap) {
                        win->input_channel_send = req.transferred_cap;
                        oz::sys_debug_print("[WM] BIND_INPUT_CHANNEL: successfully bound input channel!\n\r");
                        resp.data[0] = 0;
                    } else {
                        resp.data[0] = 1;
                    }
                    break;
                }
                case oz::ipc::WMCmd::MINIMIZE_WINDOW: {
                    bool ok = g_wm.minimize_focused();
                    if (ok) {
                        sync_state_to_display();
                        resp.data[0] = 0;
                    } else {
                        resp.data[0] = 1;
                    }
                    break;
                }
                case oz::ipc::WMCmd::RESTORE_MINIMIZED: {
                    bool ok = g_wm.restore_minimized();
                    if (ok) {
                        sync_state_to_display();
                        resp.data[0] = 0;
                    } else {
                        resp.data[0] = 1;
                    }
                    break;
                }
                case oz::ipc::WMCmd::GET_WINDOW_INFO: {
                    std::uint32_t win_id = static_cast<std::uint32_t>(req.data[1]);
                    auto* win = g_wm.get_window(win_id);
                    if (win) {
                        resp.data[0] = 0;
                        resp.data[1] = win->workspace_idx;
                        resp.data[2] = win->is_minimized ? 1 : 0;
                        resp.data[3] = (g_wm.focused_window_id() == win_id) ? 1 : 0;
                    } else {
                        resp.data[0] = 1;
                    }
                    break;
                }
                default:
                    resp.data[0] = static_cast<std::uint64_t>(-1);
                    break;
            }

            oz::sys_debug_print("[WM] Main loop: preparing to reply to client...\n\r");
            if (reply_endpoint != 0) {
                bool rep_ok = oz::sys_channel_send(reply_endpoint, &resp);
                if (!rep_ok) {
                    oz::sys_debug_print("[WM] ERROR: sys_channel_send to reply_endpoint FAILED!\n\r");
                } else {
                    oz::sys_debug_print("[WM] Sent reply to client successfully!\n\r");
                }
            } else {
                oz::sys_debug_print("[WM] reply_endpoint is 0 (no reply requested)\n\r");
            }
        }
    }
}
