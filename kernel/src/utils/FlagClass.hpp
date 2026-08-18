#pragma once

#include <type_traits>

namespace utils {
template <typename FlagType, typename Derived>

class FlagClass {
    FlagType flags;
    constexpr FlagClass(FlagType flags) : flags(flags) {}
    friend Derived;
public:
    constexpr void negate() {
        flags = ~flags;
    }
    constexpr Derived operator~() const {
        Derived result(static_cast<const Derived&>(*this));
        result.negate();
        return result;
    }
    constexpr Derived& operator|=(const Derived& other) {
        flags |= other.flags;
        return static_cast<Derived&>(*this);
    }
    constexpr Derived& operator&=(const Derived& other) {
        flags &= other.flags;
        return static_cast<Derived&>(*this);
    }
    constexpr Derived& operator^=(const Derived& other) {
        flags ^= other.flags;
        return static_cast<Derived&>(*this);
    }
    friend constexpr Derived operator|(const Derived& lhs, const Derived& rhs) {
        Derived result(lhs);
        result |= rhs;
        return result;
    }
    friend constexpr Derived operator&(const Derived& lhs, const Derived& rhs) {
        Derived result(lhs);
        result &= rhs;
        return result;
    }
    friend constexpr Derived operator^(const Derived& lhs, const Derived& rhs) {
        Derived result(lhs);
        result ^= rhs;
        return result;
    }
    friend constexpr bool operator==(const Derived& lhs, const Derived& rhs) {
        return lhs.flags == rhs.flags;
    }
    friend constexpr bool operator!=(const Derived& lhs, const Derived& rhs) {
        return lhs.flags != rhs.flags;
    }
    constexpr explicit operator FlagType() const {
        return flags;
    }
};
}