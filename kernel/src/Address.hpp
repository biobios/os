#pragma once

#include <cstdint>

namespace oz {
// 0xffff800000000000 ~ 0xffff880000000000 : reserved (8TB)
// 0xffff880000000000 ~ 0xffffc80000000000 : direct map領域 (64TB)
// 0xffffc80000000000 ~ 0xffffc90000000000 : reserved (1TB)
// 0xffffc90000000000 ~ 0xffffe90000000000 : 仮想メモリ領域 (32TB)
// 0xffffe90000000000 ~ 0xffffea0000000000 : reserved (1TB)
// 0xffffea0000000000 ~ 0xffffeb0000000000 : EFI runtime services領域 (1TB)
// 0xffffeb0000000000 ~ 0xffffec0000000000 : reserved (1TB)
// 0xffffec0000000000 ~ 0xffffec8000000000 : スタック領域 (512GB)
// 0xffffec8000000000 ~ 0xffffed8000000000 : reserved (1TB)
// 0xffffee0000000000 ~ 0xffffef0000000000 : メモリマップ領域 (1TB)
// 0xffffef0000000000 ~ 0xffffff0000000000 : reserved (32TB)
// 0xffffff0000000000 ~ 0xffffff8000000000 : reserved (512GB)
// 0xffffff8000000000 ~ 0xfffffff000000000 : reserved (64 * 7GB)
// 0xfffffff000000000 ~ 0xffffffff00000000 : reserved (8 * 7GB)
// 0xffffffff00000000 ~ 0xffffffff80000000 : reserved (2GB)
// 0xffffffff80000000 ~ 0xffffffffa0000000 : カーネルコード領域 (512MB)
// 0xffffffffa0000000 ~ 0xffffffffe0000000 : カーネルモジュール領域 (1GB)
// 0xffffffffe0000000 ~ 0xffffffffffffffff : reserved (512MB)

template <typename T>
class PhysicalAddress {
private:
    std::uintptr_t value;
public:
    constexpr std::uintptr_t get() const {
        return value;
    }
private:
    constexpr PhysicalAddress(std::uintptr_t v) : value{v} {}
    friend class PhysicalAddressProvider;
};

class PhysicalAddressProvider {
protected:
    template <typename T>
    static constexpr PhysicalAddress<T> createPhysicalAddress(std::uintptr_t v) {
        return PhysicalAddress<T>{v};
    }
};

}