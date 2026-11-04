#pragma once

#include "CapabilityTypes.hpp"
#include <cstdint>

namespace oz {

enum class ChannelMode : std::uint8_t {
    BLOCKING        = 0, // 送信側: ブロッキング / 受信側: ブロッキング (従来互換)
    RECV_NONBLOCK   = 1, // 送信側: ブロッキング / 受信側: ノンブロッキング
    SEND_NONBLOCK   = 2, // 送信側: ノンブロッキング / 受信側: ブロッキング
};

constexpr std::uint32_t IPC_FLAG_BLOCK = 1;

using ReplyToken = std::uint64_t;
constexpr ReplyToken INVALID_REPLY_TOKEN = 0;

struct IPCMessage {
    std::uint64_t data[4]{};
    CapID         transferred_cap{0};
    bool          has_cap{false};
    std::uint64_t badge{0};
};

constexpr std::uint32_t IPC_RECV_SLOT_NONE = 0xFFFFFFFFU;

} // namespace oz
