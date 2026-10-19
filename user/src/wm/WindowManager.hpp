#pragma once

#include "Layout.hpp"
#include <cstdint>
#include <cstddef>

namespace oz::wm {

enum class WMMode : std::uint8_t {
    NORMAL = 0,
    RESIZE,
    LAUNCHER,
};

static constexpr std::size_t MAX_WORKSPACES     = 9;
static constexpr std::size_t MAX_WINDOWS        = 32;
static constexpr std::size_t MAX_SCRATCHPAD     = 16;
static constexpr std::uint32_t STATUS_BAR_HEIGHT = 24;
static constexpr std::uint32_t BORDER_THICKNESS = 2;

struct WindowInfo {
    std::uint32_t id{0};
    std::uint32_t surface_id{0};
    std::uint32_t input_channel_send{0};
    char title[32]{};
    std::uint8_t workspace_idx{0}; // 0-based index (0..8)
    bool is_minimized{false};
    bool was_visible{false};
    bool is_overlay{false};
    std::uint32_t parent_window_id{0}; // For sheet/dialog overlays
    Rect overlay_rect{};
};

class WindowManager {
private:
    Rect screen_bounds_{};
    Rect work_area_{}; // screen_bounds_ minus status bar

    LayoutTree workspaces_[MAX_WORKSPACES]{};
    std::uint8_t active_workspace_{0}; // 0..8 (Workspace 1..9)

    WindowInfo windows_[MAX_WINDOWS]{};
    std::uint32_t focused_win_id_{0};

    std::uint32_t scratchpad_[MAX_SCRATCHPAD]{};
    std::size_t scratchpad_count_{0};

    WMMode mode_{WMMode::NORMAL};

public:
    WindowManager() = default;

    void init(std::uint32_t screen_w, std::uint32_t screen_h) {
        screen_bounds_ = Rect{0, 0, screen_w, screen_h};
        work_area_ = Rect{0, static_cast<std::int32_t>(STATUS_BAR_HEIGHT), screen_w, screen_h - STATUS_BAR_HEIGHT};

        for (std::size_t i = 0; i < MAX_WORKSPACES; ++i) {
            workspaces_[i].init(work_area_, BORDER_THICKNESS);
        }

        for (std::size_t i = 0; i < MAX_WINDOWS; ++i) {
            windows_[i] = WindowInfo{};
        }

        active_workspace_ = 0;
        focused_win_id_ = 0;
        scratchpad_count_ = 0;
        mode_ = WMMode::NORMAL;
    }

    Rect screen_bounds() const { return screen_bounds_; }
    Rect work_area() const { return work_area_; }
    std::uint8_t active_workspace() const { return active_workspace_; }
    std::uint32_t focused_window_id() const { return focused_win_id_; }
    void set_focused_window_id(std::uint32_t id) { focused_win_id_ = id; }
    WMMode mode() const { return mode_; }
    void set_mode(WMMode m) { mode_ = m; }
    std::size_t scratchpad_count() const { return scratchpad_count_; }

    LayoutTree& current_layout() {
        return workspaces_[active_workspace_];
    }

    WindowInfo* get_window(std::uint32_t id) {
        if (id == 0) return nullptr;
        for (std::size_t i = 0; i < MAX_WINDOWS; ++i) {
            if (windows_[i].id == id) return &windows_[i];
        }
        return nullptr;
    }

    WindowInfo* get_focused_window() {
        return get_window(focused_win_id_);
    }

    // Allocate new window structure
    WindowInfo* alloc_window(std::uint32_t surface_id, std::uint32_t input_channel_send) {
        for (std::size_t i = 0; i < MAX_WINDOWS; ++i) {
            if (windows_[i].id == 0) {
                windows_[i].id = static_cast<std::uint32_t>(i + 1);
                windows_[i].surface_id = surface_id;
                windows_[i].input_channel_send = input_channel_send;
                windows_[i].workspace_idx = active_workspace_;
                windows_[i].is_minimized = false;
                windows_[i].is_overlay = false;
                windows_[i].parent_window_id = 0;
                return &windows_[i];
            }
        }
        return nullptr;
    }

    // Add window to current workspace (or next workspace if minimum size exceeded)
    bool place_window(std::uint32_t win_id) {
        WindowInfo* win = get_window(win_id);
        if (!win) return false;

        std::uint8_t ws = active_workspace_;
        LayoutNode* node = nullptr;

        // Try inserting into current workspace; if too small, search forward for next workspace
        for (std::size_t tries = 0; tries < MAX_WORKSPACES; ++tries) {
            std::uint8_t target_ws = static_cast<std::uint8_t>((ws + tries) % MAX_WORKSPACES);
            LayoutNode* target_leaf = workspaces_[target_ws].find_by_id(focused_win_id_);
            node = workspaces_[target_ws].insert_window(win_id, target_leaf);
            if (node) {
                win->workspace_idx = target_ws;
                if (target_ws != active_workspace_) {
                    // Switch to the target workspace if placed there
                    switch_workspace(target_ws);
                }
                focused_win_id_ = win_id;
                return true;
            }
        }
        return false;
    }

    // Remove window completely
    bool destroy_window(std::uint32_t win_id) {
        WindowInfo* win = get_window(win_id);
        if (!win) return false;

        // Remove from scratchpad if minimized
        if (win->is_minimized) {
            remove_from_scratchpad(win_id);
        } else if (!win->is_overlay) {
            workspaces_[win->workspace_idx].remove_window(win_id);
        }

        if (focused_win_id_ == win_id) {
            // Focus another leaf in current workspace
            LayoutNode* next = workspaces_[active_workspace_].root();
            while (next && !next->is_leaf()) {
                next = next->first;
            }
            focused_win_id_ = next ? next->id : 0;
        }

        win->id = 0;
        return true;
    }

    // Switch workspace
    void switch_workspace(std::uint8_t ws_idx) {
        if (ws_idx >= MAX_WORKSPACES) return;
        active_workspace_ = ws_idx;

        // Update focus to first window in new workspace
        LayoutNode* leaf = workspaces_[active_workspace_].root();
        while (leaf && !leaf->is_leaf()) {
            leaf = leaf->first;
        }
        focused_win_id_ = leaf ? leaf->id : 0;
    }

    // Move focused window to specific workspace
    void move_focused_to_workspace(std::uint8_t target_ws) {
        if (target_ws >= MAX_WORKSPACES || target_ws == active_workspace_) return;
        if (focused_win_id_ == 0) return;

        std::uint32_t moving_id = focused_win_id_;
        WindowInfo* win = get_window(moving_id);
        if (!win) return;

        workspaces_[active_workspace_].remove_window(moving_id);
        win->workspace_idx = target_ws;

        workspaces_[target_ws].insert_window(moving_id);

        // Update focus on current workspace
        LayoutNode* leaf = workspaces_[active_workspace_].root();
        while (leaf && !leaf->is_leaf()) {
            leaf = leaf->first;
        }
        focused_win_id_ = leaf ? leaf->id : 0;
    }

    // Minimize focused window to scratchpad
    bool minimize_focused() {
        if (focused_win_id_ == 0 || scratchpad_count_ >= MAX_SCRATCHPAD) return false;

        std::uint32_t win_id = focused_win_id_;
        WindowInfo* win = get_window(win_id);
        if (!win) return false;

        workspaces_[active_workspace_].remove_window(win_id);
        win->is_minimized = true;

        scratchpad_[scratchpad_count_++] = win_id;

        // Update focus
        LayoutNode* leaf = workspaces_[active_workspace_].root();
        while (leaf && !leaf->is_leaf()) {
            leaf = leaf->first;
        }
        focused_win_id_ = leaf ? leaf->id : 0;
        return true;
    }

    // Restore most recent window from scratchpad
    bool restore_minimized() {
        if (scratchpad_count_ == 0) return false;

        std::uint32_t win_id = scratchpad_[--scratchpad_count_];
        WindowInfo* win = get_window(win_id);
        if (!win) return false;

        win->is_minimized = false;
        win->workspace_idx = active_workspace_;
        workspaces_[active_workspace_].insert_window(win_id);
        focused_win_id_ = win_id;
        return true;
    }

    // Focus navigation via Direction (hjkl)
    void move_focus(Direction dir) {
        if (focused_win_id_ == 0) return;
        LayoutNode* cur = workspaces_[active_workspace_].find_by_id(focused_win_id_);
        if (!cur) {
            oz::sys_debug_print("[WM] move_focus: cur node not found!\n\r");
            return;
        }

        LayoutNode* neighbor = workspaces_[active_workspace_].find_neighbor(cur, dir);
        if (neighbor && neighbor->is_leaf()) {
            oz::sys_debug_print("[WM] move_focus: switched focus to neighbor id=");
            char nb_buf[4] = {static_cast<char>('0' + neighbor->id), '\n', '\r', '\0'};
            oz::sys_debug_print(nb_buf);
            focused_win_id_ = neighbor->id;
        } else {
            oz::sys_debug_print("[WM] move_focus: neighbor is null or not leaf!\n\r");
        }
    }

    // Resize focused window in resize mode
    void resize_focused(float delta) {
        if (focused_win_id_ == 0) return;
        LayoutNode* cur = workspaces_[active_workspace_].find_by_id(focused_win_id_);
        if (cur) {
            workspaces_[active_workspace_].adjust_ratio(cur, delta);
        }
    }

    // Check if a workspace has active windows
    bool workspace_has_windows(std::uint8_t ws) const {
        if (ws >= MAX_WORKSPACES) return false;
        return (workspaces_[ws].count_windows() > 0);
    }

private:
    void remove_from_scratchpad(std::uint32_t win_id) {
        for (std::size_t i = 0; i < scratchpad_count_; ++i) {
            if (scratchpad_[i] == win_id) {
                for (std::size_t j = i; j + 1 < scratchpad_count_; ++j) {
                    scratchpad_[j] = scratchpad_[j + 1];
                }
                scratchpad_count_--;
                break;
            }
        }
    }
};

} // namespace oz::wm
