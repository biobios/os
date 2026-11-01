#pragma once

#include "core/CartesianTree.hpp"

namespace oz {

inline int test_cartesian_tree_algorithms() {
    Baton a, b, c, d, e, f;
    a.is_infinite = true;
    a.quantum = ~0ULL;
    a.remaining = ~0ULL;

    b.quantum = 10;
    b.remaining = 10;

    c.quantum = 40;
    c.remaining = 40;

    d.quantum = 25;
    d.remaining = 25;

    e.quantum = 15;
    e.remaining = 15;

    f.quantum = 30;
    f.remaining = 30;

    Baton* root = nullptr;
    cartesianPush(root, nullptr, &a);
    if (root != &a) return 1;

    cartesianPush(root, &a, &b);
    if (root != &b || b.cartesian_left != &a) return 2;

    cartesianPush(root, &b, &c);
    if (root != &b || b.cartesian_right != &c) return 3;

    cartesianPush(root, &c, &d);
    cartesianPush(root, &d, &e);
    cartesianPush(root, &e, &f);

    if (root != &b) return 4;
    if (b.cartesian_left != &a) return 5;
    if (b.cartesian_right != &e) return 6;
    if (e.cartesian_left != &d) return 7;
    if (d.cartesian_left != &c) return 8;
    if (e.cartesian_right != &f) return 9;
    if (f.cartesian_right != nullptr) return 10;

    cartesianPopLeaf(root, &f);
    if (e.cartesian_right != nullptr) return 11;

    Baton* sub_root = cartesianSplitAt(root, &b, 5);
    if (root != &a) return 12;
    if (sub_root != &e) return 13;
    if (sub_root->lazy_delta != 5) return 14;
    if (sub_root->get_effective_remaining() != 10) return 15;

    cartesianPushDown(sub_root);
    if (sub_root->remaining != 10 || sub_root->lazy_delta != 0) return 16;
    if (d.lazy_delta != 5) return 17;
    if (d.get_effective_remaining() != 20) return 18;

    Baton* merged = cartesianZipMerge(root, sub_root);
    if (merged != &e) return 19;
    if (e.cartesian_left != &d) return 20;
    if (d.cartesian_left != &c) return 21;
    if (c.cartesian_left != &a) return 22;
    if (a.cartesian_parent != &c) return 23;

    return 0;
}

} // namespace oz
