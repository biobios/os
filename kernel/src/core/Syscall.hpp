#pragma once
#include <cstdint>

namespace oz {

class CSpace;
class ExecutionContext;
class Thread;
class UntypedMemory;

extern void (*g_exit_thread_func)();
extern CSpace* (*g_get_current_cspace_func)();

// For blocking IPC
extern Thread* (*g_get_current_thread_func)();
extern ExecutionContext* (*g_get_current_context_func)();
extern void (*g_block_thread_func)();           // Block current thread and switch to next
extern void (*g_unblock_thread_func)(Thread*);   // Wake up a blocked thread

// Queues a thread into the scheduler
extern void (*g_queue_thread_func)(Thread* t);

// ユーザースレッドとカーネルのスタックを切り替えるためのコンテキスト
struct SyscallContext {
    std::uint64_t user_rsp;
    std::uint64_t kernel_rsp;
};

void initSyscall();
void setSyscallKernelStack(std::uint64_t rsp);

extern SyscallContext g_syscall_context;

} // namespace oz
