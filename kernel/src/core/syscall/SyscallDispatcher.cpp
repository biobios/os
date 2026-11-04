#include "core/syscall/SyscallHandlers.hpp"

namespace oz {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type-c-linkage"
extern "C" SyscallResult handleSyscall(std::uint64_t rdi, std::uint64_t rsi, std::uint64_t rdx,
                                       std::uint64_t r10, std::uint64_t r8, std::uint64_t r9,
                                       std::uint64_t syscall_number) {
#pragma GCC diagnostic pop

    ScopeTlbFlusher tlb_flusher;
    SyscallArgs args{rdi, rsi, rdx, r10, r8, r9, static_cast<Syscall>(syscall_number)};

    switch (args.number) {
        // Channel IPC
        case Syscall::ChannelCreate:
        case Syscall::Call:
        case Syscall::ReplyAndRecv:
        case Syscall::ChannelBindNotification:
            return handleSyscallChannel(args);

        // Capability Operations
        case Syscall::CapMint:
        case Syscall::CapDelete:
        case Syscall::CapCopy:
        case Syscall::CapMove:
        case Syscall::CapMoveRemote:
        case Syscall::CapDerive:
        case Syscall::CapRevoke:
        case Syscall::CapIdentify:
            return handleSyscallCapability(args);

        // Memory & Paging & Untyped
        case Syscall::PageTableMapFrame:
        case Syscall::PageTableUnmap:
        case Syscall::FrameGetPhys:
        case Syscall::PageTableCreate:
        case Syscall::UntypedRetype:
        case Syscall::PageTableMapChild:
        case Syscall::FrameGetSize:
        case Syscall::UntypedRetypeBulk:
            return handleSyscallMemory(args);

        // Process, Thread, Baton, Notification & Sched
        case Syscall::Exit:
        case Syscall::TcbConfigure:
        case Syscall::TcbResume:
        case Syscall::TcbSuspend:
        case Syscall::TcbSetFaultEndpoint:
        case Syscall::NotificationSignal:
        case Syscall::NotificationWait:
        case Syscall::NotificationPoll:
        case Syscall::TcbBindNotification:
        case Syscall::TcbGetBudget:
        case Syscall::TcbGetConsumption:
        case Syscall::BatonSwitch:
        case Syscall::BatonInject:
        case Syscall::Yield:
        case Syscall::TcbGetId:
        case Syscall::TcbSetTimeoutNotification:
        case Syscall::BatonSetTimeoutNotification:
        case Syscall::TcbBindBaton:
        case Syscall::NotificationDelegate:
        case Syscall::NotificationUndelegate:
        case Syscall::BatonCreateIrq:
        case Syscall::BatonSetBudget:
            return handleSyscallProcess(args);

        // Hardware, IO & IRQ
        case Syscall::Write:
        case Syscall::IrqHandlerCreate:
        case Syscall::IrqAckLegacy:
        case Syscall::IOPortCreate:
        case Syscall::IOPortIn:
        case Syscall::IOPortOut:
        case Syscall::IrqAck:
        case Syscall::IrqBindNotification:
        case Syscall::IrqWait:
            return handleSyscallHardware(args);

        default:
            return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
    }
}

} // namespace oz
