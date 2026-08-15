#pragma once

#include <cstddef>
#include <cstdint>

namespace oz {

// Virtual address layout constants
constexpr std::uintptr_t DIRECT_MAP_OFFSET = 0xFFFF880000000000ULL;
constexpr std::uintptr_t KERNEL_VIRT_OFFSET = 0xFFFFFFFF80000000ULL;

template <typename T = void>
class PhysicalAddress;

class PhysicalAddressProvider {
protected:
    template <typename T = void>
    static constexpr PhysicalAddress<T> createPhysicalAddress(std::uintptr_t v) {
        return PhysicalAddress<T>{v};
    }

    template <typename T = void>
    static constexpr PhysicalAddress<T> offsetPhysicalAddress(PhysicalAddress<T> p, std::ptrdiff_t offset) {
        return PhysicalAddress<T>{p.value + offset};
    }

    template <typename T = void>
    static constexpr PhysicalAddress<T> nullPhysicalAddress() {
        return PhysicalAddress<T>{0};
    }
};

template <typename T>
class PhysicalAddress {
private:
    std::uintptr_t value;

    constexpr explicit PhysicalAddress(std::uintptr_t v) : value{v} {}
    constexpr PhysicalAddress() : value{0} {}

public:
    constexpr PhysicalAddress(const PhysicalAddress&) = default;
    constexpr PhysicalAddress& operator=(const PhysicalAddress&) = default;

    constexpr std::uintptr_t get() const { return value; }
    constexpr explicit operator bool() const { return value != 0; }
    constexpr bool operator==(const PhysicalAddress& other) const { return value == other.value; }
    constexpr bool operator!=(const PhysicalAddress& other) const { return value != other.value; }
    constexpr bool operator<(const PhysicalAddress& other) const { return value < other.value; }
    constexpr bool operator<=(const PhysicalAddress& other) const { return value <= other.value; }
    constexpr bool operator>(const PhysicalAddress& other) const { return value > other.value; }
    constexpr bool operator>=(const PhysicalAddress& other) const { return value >= other.value; }

    constexpr std::ptrdiff_t operator-(const PhysicalAddress& other) const {
        return static_cast<std::ptrdiff_t>(value - other.value);
    }

    friend class PhysicalAddressProvider;

    template <typename U>
    friend inline U* phys_to_virt(PhysicalAddress<U> phys_addr);

    template <typename U>
    friend inline PhysicalAddress<U> virt_to_phys_direct(U* virt_addr);

    template <typename To, typename From>
    friend inline PhysicalAddress<To> physical_address_cast(PhysicalAddress<From> phys);
};

// physical_address_cast for reinterpreting the underlying pointee type of PhysicalAddress
template <typename To, typename From>
inline PhysicalAddress<To> physical_address_cast(PhysicalAddress<From> phys) {
    return PhysicalAddress<To>{phys.value};
}

// Convert PhysicalAddress<T> to direct-mapped virtual address T*
template <typename T>
inline T* phys_to_virt(PhysicalAddress<T> phys_addr) {
    return reinterpret_cast<T*>(phys_addr.get() + DIRECT_MAP_OFFSET);
}

// Convert direct-mapped virtual address T* to PhysicalAddress<T>
template <typename T>
inline PhysicalAddress<T> virt_to_phys_direct(T* virt_addr) {
    return PhysicalAddress<T>{reinterpret_cast<std::uintptr_t>(virt_addr) - DIRECT_MAP_OFFSET};
}

} // namespace oz