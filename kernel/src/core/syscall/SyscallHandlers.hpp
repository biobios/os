#pragma once

#include "core/syscall/SyscallCommon.hpp"

namespace oz {

SyscallResult handleSyscallChannel(const SyscallArgs& args);
SyscallResult handleSyscallMemory(const SyscallArgs& args);
SyscallResult handleSyscallProcess(const SyscallArgs& args);
SyscallResult handleSyscallCapability(const SyscallArgs& args);
SyscallResult handleSyscallHardware(const SyscallArgs& args);

} // namespace oz
