#pragma once
#include <cstdint>
#include <cstddef>
#include <type_traits>

struct Aligned1 {
    using data_t = std::uint8_t;
};

struct Aligned2 {
    using data_t = std::uint16_t;
};

struct Aligned4 {
    using data_t = std::uint32_t;
};

template <typename T, typename Aligned>
    requires (sizeof(T) >= sizeof(typename Aligned::data_t))
struct Unaligned {
    using data_t = typename Aligned::data_t;
    static constexpr std::size_t alignment = sizeof(data_t);
    static constexpr std::size_t size = sizeof(T);
    static constexpr std::size_t num_elements = sizeof(T) / sizeof(data_t);
    data_t data[num_elements];

    constexpr Unaligned() = default;

    constexpr Unaligned(T val) : data{} {
        for (auto i = 0uz; i < num_elements; ++i) {
            data[i] = static_cast<data_t>(val >> (i * alignment * 8));
        }
    }

    template <typename Self>
    constexpr operator T(this Self& self) {
        T result = 0;
        for (auto i = 0uz; i < num_elements; ++i) {
            result |= static_cast<T>(self.data[i]) << (i * alignment * 8);
        }
        return result;
    }

    template <typename Self>
        requires (!std::is_const_v<Self>)
    constexpr Self& operator=(this Self& self, T val) {
        for (auto i = 0uz; i < num_elements; ++i) {
            self.data[i] = static_cast<data_t>(val >> (i * alignment * 8));
        }
        return self;
    }

    template <typename Self>
        requires (!std::is_const_v<Self>)
    constexpr Self& operator|=(this Self& self, T val) {
        for (auto i = 0uz; i < num_elements; ++i) {
            self.data[i] |= static_cast<data_t>(val >> (i * alignment * 8));
        }
        return self;
    }
    
    template <typename Self>
        requires (!std::is_const_v<Self>)
    constexpr Self& operator&=(this Self& self, T val) {
        for (auto i = 0uz; i < num_elements; ++i) {
            self.data[i] &= static_cast<data_t>(val >> (i * alignment * 8));
        }
        return self;
    }
};