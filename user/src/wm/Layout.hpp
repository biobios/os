#pragma once

#include <cstdint>
#include <cstddef>

namespace oz::wm {

struct Rect {
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint32_t w{0};
    std::uint32_t h{0};

    constexpr bool contains(std::int32_t px, std::int32_t py) const {
        return (px >= x && px < static_cast<std::int32_t>(x + w) &&
                py >= y && py < static_cast<std::int32_t>(y + h));
    }
};

enum class SplitType : std::uint8_t {
    NONE = 0,
    VERTICAL,   // Split into left and right
    HORIZONTAL, // Split into top and bottom
};

enum class Direction : std::uint8_t {
    LEFT = 0,
    RIGHT,
    UP,
    DOWN
};

static constexpr std::uint32_t MIN_WINDOW_WIDTH  = 200;
static constexpr std::uint32_t MIN_WINDOW_HEIGHT = 150;
static constexpr std::size_t MAX_NODES_PER_TREE  = 64;

class LayoutNode {
public:
    bool in_use{false};            // Tracking allocation state in pool
    std::uint32_t id{0};           // Window ID (0 if internal split node)
    SplitType split{SplitType::NONE};
    float ratio{0.5f};             // Split ratio (0.1f .. 0.9f)
    Rect rect{};

    LayoutNode* parent{nullptr};
    LayoutNode* first{nullptr};    // Left (if vertical) or Top (if horizontal)
    LayoutNode* second{nullptr};   // Right (if vertical) or Bottom (if horizontal)

    constexpr bool is_leaf() const {
        return (first == nullptr && second == nullptr);
    }
};

class LayoutTree {
private:
    LayoutNode pool_[MAX_NODES_PER_TREE]{};
    std::size_t pool_used_{0};
    LayoutNode* root_{nullptr};
    Rect bounds_{};
    std::uint32_t border_width_{2};

    LayoutNode* alloc_node() {
        for (std::size_t i = 0; i < MAX_NODES_PER_TREE; ++i) {
            if (!pool_[i].in_use) {
                pool_[i] = LayoutNode{};
                pool_[i].in_use = true;
                return &pool_[i];
            }
        }
        return nullptr;
    }

    void free_node(LayoutNode* node) {
        if (!node) return;
        node->in_use = false;
        node->id = 0;
        node->split = SplitType::NONE;
        node->ratio = 0.5f;
        node->rect = Rect{};
        node->parent = nullptr;
        node->first = nullptr;
        node->second = nullptr;
    }

    void update_geometry(LayoutNode* node, Rect r) {
        if (!node) return;
        node->rect = r;

        if (node->is_leaf()) {
            return;
        }

        if (node->split == SplitType::VERTICAL) {
            std::uint32_t w1 = static_cast<std::uint32_t>(r.w * node->ratio);
            std::uint32_t w2 = r.w - w1;
            Rect r1{r.x, r.y, w1, r.h};
            Rect r2{static_cast<std::int32_t>(r.x + w1), r.y, w2, r.h};
            update_geometry(node->first, r1);
            update_geometry(node->second, r2);
        } else if (node->split == SplitType::HORIZONTAL) {
            std::uint32_t h1 = static_cast<std::uint32_t>(r.h * node->ratio);
            std::uint32_t h2 = r.h - h1;
            Rect r1{r.x, r.y, r.w, h1};
            Rect r2{r.x, static_cast<std::int32_t>(r.y + h1), r.w, h2};
            update_geometry(node->first, r1);
            update_geometry(node->second, r2);
        }
    }

public:
    LayoutTree() = default;

    void init(Rect bounds, std::uint32_t border_width = 2) {
        bounds_ = bounds;
        border_width_ = border_width;
        root_ = nullptr;
        for (std::size_t i = 0; i < MAX_NODES_PER_TREE; ++i) {
            free_node(&pool_[i]);
        }
    }

    void set_bounds(Rect bounds) {
        bounds_ = bounds;
        if (root_) {
            update_geometry(root_, bounds_);
        }
    }

    Rect bounds() const { return bounds_; }
    LayoutNode* root() const { return root_; }

    LayoutNode* find_by_id(std::uint32_t win_id) {
        if (win_id == 0 || !root_) return nullptr;
        return find_recursive(root_, win_id);
    }

    // Check if adding a new window to target node would violate minimum size
    bool can_split(LayoutNode* target) const {
        if (!target) {
            // First window in empty tree
            return (bounds_.w >= MIN_WINDOW_WIDTH && bounds_.h >= MIN_WINDOW_HEIGHT);
        }
        Rect r = target->rect;
        if (r.w >= r.h) {
            // Will split vertically
            return ((r.w / 2) >= MIN_WINDOW_WIDTH && r.h >= MIN_WINDOW_HEIGHT);
        } else {
            // Will split horizontally
            return (r.w >= MIN_WINDOW_WIDTH && (r.h / 2) >= MIN_WINDOW_HEIGHT);
        }
    }

    // Add a new window into the tree
    // If target is null or empty tree, becomes root.
    // If target is specified, splits target.
    LayoutNode* insert_window(std::uint32_t win_id, LayoutNode* target = nullptr) {
        if (win_id == 0) return nullptr;

        if (!root_) {
            root_ = alloc_node();
            if (!root_) return nullptr;
            root_->id = win_id;
            root_->rect = bounds_;
            return root_;
        }

        if (!target) {
            // Fallback: split the first leaf found
            target = find_first_leaf(root_);
            if (!target) return nullptr;
        }

        if (!can_split(target)) {
            return nullptr; // Exceeds minimum size constraint
        }

        LayoutNode* new_leaf = alloc_node();
        LayoutNode* copy_leaf = alloc_node();
        if (!new_leaf || !copy_leaf) {
            if (new_leaf) free_node(new_leaf);
            if (copy_leaf) free_node(copy_leaf);
            return nullptr;
        }

        oz::sys_debug_print("[WM] insert_window: win_id=");
        char w_buf[4] = {static_cast<char>('0' + win_id), '\n', '\r', '\0'};
        oz::sys_debug_print(w_buf);
        oz::sys_debug_print("[WM] target->id=");
        char t_buf[4] = {static_cast<char>('0' + target->id), '\n', '\r', '\0'};
        oz::sys_debug_print(t_buf);

        // Copy existing target's leaf content into copy_leaf
        copy_leaf->id = target->id;
        copy_leaf->parent = target;

        // Setup new leaf
        new_leaf->id = win_id;
        new_leaf->parent = target;

        // Convert target into an internal split node
        target->id = 0; // Internal node has no win_id
        if (target->rect.w >= target->rect.h) {
            target->split = SplitType::VERTICAL;
        } else {
            target->split = SplitType::HORIZONTAL;
        }
        target->ratio = 0.5f;
        target->first = copy_leaf;
        target->second = new_leaf;

        // Recalculate geometries
        update_geometry(target, target->rect);

        return new_leaf;
    }

    // Remove window and collapse tree
    bool remove_window(std::uint32_t win_id) {
        LayoutNode* node = find_by_id(win_id);
        if (!node) return false;

        if (node == root_) {
            // Single window at root
            free_node(root_);
            root_ = nullptr;
            return true;
        }

        LayoutNode* parent = node->parent;
        if (!parent) return false;

        LayoutNode* sibling = (parent->first == node) ? parent->second : parent->first;
        if (!sibling) return false;

        LayoutNode* grand_parent = parent->parent;

        if (!grand_parent) {
            // Parent was root, sibling becomes root
            sibling->parent = nullptr;
            root_ = sibling;
            free_node(node);
            free_node(parent);
            update_geometry(root_, bounds_);
            return true;
        }

        // Connect sibling to grandparent
        sibling->parent = grand_parent;
        if (grand_parent->first == parent) {
            grand_parent->first = sibling;
        } else {
            grand_parent->second = sibling;
        }

        free_node(node);
        free_node(parent);
        update_geometry(grand_parent, grand_parent->rect);
        return true;
    }

    // Find next window in direction (LEFT, RIGHT, UP, DOWN)
    LayoutNode* find_neighbor(LayoutNode* current, Direction dir) {
        if (!current || !root_) {
            oz::sys_debug_print("[WM] find_neighbor: null current or root\n\r");
            return nullptr;
        }

        Rect cur_r = current->rect;
        std::int32_t cur_cx = cur_r.x + static_cast<std::int32_t>(cur_r.w / 2);
        std::int32_t cur_cy = cur_r.y + static_cast<std::int32_t>(cur_r.h / 2);

        LayoutNode* best = nullptr;
        std::uint64_t best_dist = 0xFFFFFFFFFFFFFFFFULL;

        find_neighbor_recursive(root_, current, dir, cur_cx, cur_cy, best, best_dist);
        return best;
    }

    // Adjust split ratio for resizing
    bool adjust_ratio(LayoutNode* leaf, float delta) {
        if (!leaf || !leaf->parent) return false;
        LayoutNode* p = leaf->parent;
        float new_ratio = p->ratio + delta;
        if (new_ratio < 0.1f) new_ratio = 0.1f;
        if (new_ratio > 0.9f) new_ratio = 0.9f;
        p->ratio = new_ratio;
        update_geometry(p, p->rect);
        return true;
    }

    // Count active leaves
    std::size_t count_windows() const {
        if (!root_) return 0;
        return count_leaves(root_);
    }

private:
    LayoutNode* find_recursive(LayoutNode* node, std::uint32_t win_id) {
        if (!node) return nullptr;
        if (node->is_leaf()) {
            return (node->id == win_id) ? node : nullptr;
        }
        LayoutNode* res = find_recursive(node->first, win_id);
        if (res) return res;
        return find_recursive(node->second, win_id);
    }

    LayoutNode* find_first_leaf(LayoutNode* node) {
        if (!node) return nullptr;
        if (node->is_leaf()) return node;
        return find_first_leaf(node->first);
    }

    std::size_t count_leaves(LayoutNode* node) const {
        if (!node) return 0;
        if (node->is_leaf()) return 1;
        return count_leaves(node->first) + count_leaves(node->second);
    }

    void find_neighbor_recursive(LayoutNode* node, LayoutNode* current, Direction dir,
                                 std::int32_t cur_cx, std::int32_t cur_cy,
                                 LayoutNode*& best, std::uint64_t& best_dist) {
        if (!node) return;
        if (node->is_leaf()) {
            oz::sys_debug_print("[WM] find_neighbor leaf: id=");
            char id_b[4] = {static_cast<char>('0' + node->id), '\n', '\r', '\0'};
            oz::sys_debug_print(id_b);
            if (node == current) {
                oz::sys_debug_print("[WM] leaf is current\n\r");
                return;
            }

            Rect r = node->rect;
            std::int32_t cx = r.x + static_cast<std::int32_t>(r.w / 2);
            std::int32_t cy = r.y + static_cast<std::int32_t>(r.h / 2);

            bool valid = false;
            switch (dir) {
                case Direction::LEFT:  valid = (cx < cur_cx); break;
                case Direction::RIGHT: valid = (cx > cur_cx); break;
                case Direction::UP:    valid = (cy < cur_cy); break;
                case Direction::DOWN:  valid = (cy > cur_cy); break;
            }

            if (valid) {
                oz::sys_debug_print("[WM] leaf valid!\n\r");
                std::int64_t dx = cx - cur_cx;
                std::int64_t dy = cy - cur_cy;
                std::uint64_t dist = static_cast<std::uint64_t>(dx * dx + dy * dy);
                if (dist < best_dist) {
                    best_dist = dist;
                    best = node;
                }
            } else {
                oz::sys_debug_print("[WM] leaf not valid for dir\n\r");
            }
            return;
        }

        find_neighbor_recursive(node->first, current, dir, cur_cx, cur_cy, best, best_dist);
        find_neighbor_recursive(node->second, current, dir, cur_cx, cur_cy, best, best_dist);
    }
};

} // namespace oz::wm
