#pragma once

#include "core/Baton.hpp"

namespace oz {

/**
 * @brief 遅延タグの伝播 (Push-Down: 完全最悪 O(1))
 * 親ノードを訪問・操作する際、そのノードに溜まった lazy_delta を実記録時間に反映し、
 * 左右の子ノードに伝播する。
 */
inline void cartesianPushDown(Baton* node) {
    if (!node || node->lazy_delta == 0) return;
    std::uint64_t delta = node->lazy_delta;
    if (!node->is_infinite) {
        node->remaining = (node->remaining > delta) ? (node->remaining - delta) : 0;
    }
    if (node->cartesian_left)  node->cartesian_left->lazy_delta += delta;
    if (node->cartesian_right) node->cartesian_right->lazy_delta += delta;
    node->lazy_delta = 0;
}

/**
 * @brief 単調スタックによる末尾へのインクリメンタル Push (償却 O(1))
 * 新規バトン target をチェーン末尾に追加する処理。
 * 直前の末尾ノード running から親ポインタを遡って局所的にポインタを付け替える。
 */
inline void cartesianPush(Baton*& root, Baton* running, Baton* target) {
    target->resetCartesian();
    if (!running) {
        root = target;
        return;
    }

    Baton* curr = running;
    Baton* last_popped = nullptr;

    while (curr && curr->get_effective_remaining() > target->get_effective_remaining()) {
        cartesianPushDown(curr);
        last_popped = curr;
        curr = curr->cartesian_parent;
    }

    if (curr) {
        cartesianPushDown(curr);
        target->cartesian_parent = curr;
        target->cartesian_left = last_popped;
        if (last_popped) last_popped->cartesian_parent = target;
        curr->cartesian_right = target;
    } else {
        target->cartesian_parent = nullptr;
        target->cartesian_left = root;
        if (root) root->cartesian_parent = target;
        root = target;
    }
}

/**
 * @brief 末尾ノードのインクリメンタル Pop (完全最悪 O(1))
 * 正常終了 (EXIT)、自発的ブロック (BLOCKED)、下位タイムアウト時に末尾バトンを木から外す処理。
 * 末尾ノードは右の子を絶対に持たないため、左の子を親の右の子に付け替えるだけで完了する。
 */
inline void cartesianPopLeaf(Baton*& root, Baton* leaf) {
    if (!leaf) return;
    cartesianPushDown(leaf);
    Baton* child = leaf->cartesian_left;
    Baton* p = leaf->cartesian_parent;

    if (p) {
        p->cartesian_right = child;
        if (child) child->cartesian_parent = p;
    } else {
        root = child;
        if (child) child->cartesian_parent = nullptr;
    }
    leaf->resetCartesian();
}

/**
 * @brief 上位タイムアウトによる木のインクリメンタル分断 (完全最悪 O(1))
 * タイムアウトした根ノード expired で木を親側と子側に O(1) 分断する。
 * サブツリーの根ノードに経過時間を遅延タグ (lazy_delta) として O(1) 付与する。
 * @return 分断されたサブツリーの根 (expired->cartesian_right)
 */
inline Baton* cartesianSplitAt(Baton*& root, Baton* expired, std::uint64_t elapsed) {
    if (!expired) return nullptr;
    cartesianPushDown(expired);
    Baton* caller_root = expired->cartesian_left;
    Baton* sub_root = expired->cartesian_right;

    if (caller_root) caller_root->cartesian_parent = nullptr;
    if (sub_root) {
        sub_root->cartesian_parent = nullptr;
        sub_root->lazy_delta += elapsed;
    }

    expired->resetCartesian();
    root = caller_root;
    return sub_root;
}

/**
 * @brief 厳密なデカルト木マージ (ZipMerge: 通常 O(1) / スパイン上限 O(M))
 * left_tree の右スパインと right_tree の左スパインを縫い合わせて Min-Heap 性と BST 性を完全保証する。
 * 反復ループ（ノンリカーシブ）版。
 */
inline Baton* cartesianZipMerge(Baton* left_tree, Baton* right_tree) {
    if (!left_tree) return right_tree;
    if (!right_tree) return left_tree;

    Baton* root = nullptr;
    Baton** link = &root;
    Baton* parent = nullptr;

    while (left_tree && right_tree) {
        cartesianPushDown(left_tree);
        cartesianPushDown(right_tree);

        if (left_tree->get_effective_remaining() <= right_tree->get_effective_remaining()) {
            *link = left_tree;
            left_tree->cartesian_parent = parent;
            parent = left_tree;
            link = &(left_tree->cartesian_right);
            left_tree = left_tree->cartesian_right;
        } else {
            *link = right_tree;
            right_tree->cartesian_parent = parent;
            parent = right_tree;
            link = &(right_tree->cartesian_left);
            right_tree = right_tree->cartesian_left;
        }
    }

    Baton* remainder = left_tree ? left_tree : right_tree;
    *link = remainder;
    if (remainder) remainder->cartesian_parent = parent;

    return root;
}

} // namespace oz
