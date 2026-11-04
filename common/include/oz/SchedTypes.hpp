#pragma once

#include "ErrorCodes.hpp"
#include <cstdint>

namespace oz {

enum class BatonState : std::uint8_t {
    READY       = 0,
    RUNNING     = 1,
    WAIT_RETURN = 2,
    WAIT_START  = 3,
    BLOCKED     = 4
};

enum class SchedReason : std::uint64_t {
    NONE      = 0,
    TIMEOUT   = 1,
    YIELD     = 2,
    BLOCKED   = 3,
    EXIT      = 4,
    PREEMPTED = 5,
    FAULT     = 6,
    ORPHANED  = 7
};

constexpr SchedReason SYS_BATON_SWITCH_TIMEOUT = SchedReason::TIMEOUT;
constexpr SchedReason SYS_BATON_SWITCH_INVALID = static_cast<SchedReason>(SYS_ERR_INVALID);

struct SchedResult {
    SchedReason reason{SchedReason::NONE};
    union {
        std::uint64_t consumed_cycles{0}; // PREEMPTED時は時間切れとなったirq_vector
        std::uint64_t consumed_us;
    };
};

} // namespace oz
