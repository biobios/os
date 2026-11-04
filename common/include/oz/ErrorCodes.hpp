#pragma once

#include <cstdint>

namespace oz {

constexpr int SYS_ERR_INVALID       = -1;
constexpr int SYS_ERR_WOULDBLOCK    = -2;
constexpr int SYS_ERR_CANCELLED     = -3;
constexpr int SYS_ERR_DEADLK        = -4;
constexpr int SYS_ERR_INVALID_TYPE  = -5;
constexpr int SYS_ERR_BUSY          = -6;
constexpr int SYS_ERR_INVALID_STATE = -7;
constexpr int SYS_ERR_PERM          = -8;
constexpr int SYS_ERR_NO_MEMORY     = -9;

constexpr std::uint64_t TIMEOUT_BADGE          = 1ULL << 61;
constexpr std::uint64_t BUDGET_EXHAUSTED_BADGE = 1ULL << 62;

} // namespace oz
