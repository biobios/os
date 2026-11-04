#pragma once

#include <cstdint>
#include <cstddef>

namespace oz {

using CapID = std::uint32_t;
inline constexpr CapID INVALID_CAP_ID = 0xFFFFFFFF;

enum class CapRights : std::uint32_t {
    NONE        = 0,

    // Common Rights (bit 0..15)
    DUPLICATE   = 1U << 0,
    TRANSFER    = 1U << 1,
    DERIVE      = 1U << 2,
    REVOKE      = 1U << 3,

    // Type Rights (bit 16..31)
    READ        = 1U << 16,
    RECEIVE     = 1U << 16,
    WAIT        = 1U << 16,
    CONFIGURE   = 1U << 16,
    SWITCH      = 1U << 16,
    IN          = 1U << 16,
    INSERT      = 1U << 16,

    WRITE       = 1U << 17,
    SEND        = 1U << 17,
    SIGNAL      = 1U << 17,
    CONTROL     = 1U << 17,
    OUT         = 1U << 17,
    ACK_WAIT    = 1U << 17,
    DELETE      = 1U << 17,

    EXEC        = 1U << 18,
    GRANT       = 1U << 18,
    BIND        = 1U << 18,
    SET_BUDGET  = 1U << 18,

    NOCACHE     = 1U << 19,
    GRANT_REPLY = 1U << 19,
    DEBUG       = 1U << 19,

    USE         = 1U << 4, // 互換用

    ALL         = 0xFFFFFFFFU,
};

inline constexpr CapRights operator|(CapRights a, CapRights b) {
    return static_cast<CapRights>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline constexpr CapRights operator&(CapRights a, CapRights b) {
    return static_cast<CapRights>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

inline constexpr CapRights operator~(CapRights a) {
    return static_cast<CapRights>(~static_cast<std::uint32_t>(a));
}

enum class CapType : std::uint8_t {
    Null            = 0,
    Unknown         = 0, // 互換用エイリアス
    // カテゴリ A: インライン型 / KernelObject 不要型
    UntypedMemory   = 1,
    IOPort          = 2,
    IRQBatonControl = 3,
    IRQHandler      = 4,
    Frame           = 5,
    PageTable       = 6,
    // カテゴリ B: KernelObject 参照型
    ChannelEndpoint = 7,
    Notification    = 8,
    Baton           = 9,
    TCB             = 10,
    CSpaceCap       = 11,
    VSpace          = 12,
};

// 互換性エイリアス: ObjectType を CapType と一本化
using ObjectType = CapType;
using KernelCapType = CapType;

inline constexpr CapType to_cap_type(CapType t) {
    return t;
}

inline constexpr CapType to_cap_type(std::uint32_t val) {
    if (val >= 1 && val <= 12) {
        return static_cast<CapType>(val);
    }
    return CapType::Null;
}

inline constexpr ObjectType to_object_type(CapType t) {
    return t;
}

// System reserved capability slots and well-known slots
constexpr CapID CAP_PREWIRED_START        = 1;
constexpr CapID CAP_PREWIRED_END          = 63;
constexpr CapID CAP_PT_DELEGATE_START     = 64;
constexpr CapID CAP_PT_DELEGATE_END       = 127;
constexpr CapID CAP_RESERVED_SYSTEM_START = 200;
constexpr CapID CAP_RESERVED_SYSTEM_END   = 249;

inline bool is_reserved_system_slot(CapID slot) {
    return (slot >= CAP_RESERVED_SYSTEM_START && slot <= CAP_RESERVED_SYSTEM_END);
}

enum class WellKnownSlot : CapID {
    SelfVSpace   = 0,
    Untyped      = 240,
    AhciMmio     = 241,
    XhciMmio     = 242,
    BootInfo     = 243,
    InitRd       = 244,
    SelfCSpace   = 245,
    FrameBuffer  = 246,
    RootIOPort   = 247,
    RootBaton    = 248,
    IrqControl   = 249,
};

inline constexpr CapID CAP_SELF_VSPACE  = static_cast<CapID>(WellKnownSlot::SelfVSpace);
inline constexpr CapID CAP_UNTYPED      = static_cast<CapID>(WellKnownSlot::Untyped);
inline constexpr CapID CAP_AHCI_MMIO    = static_cast<CapID>(WellKnownSlot::AhciMmio);
inline constexpr CapID CAP_XHCI_MMIO    = static_cast<CapID>(WellKnownSlot::XhciMmio);
inline constexpr CapID CAP_BOOTINFO     = static_cast<CapID>(WellKnownSlot::BootInfo);
inline constexpr CapID CAP_INITRD       = static_cast<CapID>(WellKnownSlot::InitRd);
inline constexpr CapID CAP_SELF_CSPACE  = static_cast<CapID>(WellKnownSlot::SelfCSpace);
inline constexpr CapID CAP_FRAMEBUFFER  = static_cast<CapID>(WellKnownSlot::FrameBuffer);
inline constexpr CapID CAP_ROOT_IOPORT  = static_cast<CapID>(WellKnownSlot::RootIOPort);
inline constexpr CapID CAP_ROOT_BATON   = static_cast<CapID>(WellKnownSlot::RootBaton);
inline constexpr CapID CAP_IRQ_CONTROL  = static_cast<CapID>(WellKnownSlot::IrqControl);
inline constexpr CapID CAP_NULL         = 0;

inline constexpr std::uint8_t CAP_TYPE_FRAME     = static_cast<std::uint8_t>(CapType::Frame);
inline constexpr std::uint8_t CAP_TYPE_PAGETABLE = static_cast<std::uint8_t>(CapType::PageTable);

struct CapInfo {
    std::uint8_t  type{0};
    std::uint8_t  level{0};
    std::uint8_t  is_mapped{0};
    std::uint8_t  padding{0};
    std::uint32_t rights{0};
    std::uint64_t size{0};
};

} // namespace oz
