#pragma once
#include <cstdint>

namespace oz {

class CSpace;
class Thread;
class UntypedMemory;

extern void (*g_exit_thread_func)();
extern CSpace* (*g_get_current_cspace_func)();

// For blocking IPC
extern Thread* (*g_get_current_thread_func)();
extern void (*g_block_thread_func)();           // Block current thread and switch to next
extern void (*g_unblock_thread_func)(Thread*);   // Wake up a blocked thread

// Queues a thread into the scheduler
extern void (*g_queue_thread_func)(Thread* t);

// For hardware resource management (Phase 4)
class IRQHandler;
extern void (*g_register_irq_handler_fn)(std::uint8_t vector, IRQHandler* handler);
extern void (*g_eoi_fn)();
// I/O port function pointers
extern std::uint8_t (*g_read_io8_fn)(std::uint16_t);
extern std::uint16_t (*g_read_io16_fn)(std::uint16_t);
extern std::uint32_t (*g_read_io32_fn)(std::uint16_t);
extern void (*g_write_io8_fn)(std::uint16_t, std::uint8_t);
extern void (*g_write_io16_fn)(std::uint16_t, std::uint16_t);
extern void (*g_write_io32_fn)(std::uint16_t, std::uint32_t);

// ユーザースレッドとカーネルのスタックを切り替えるためのコンテキスト
struct SyscallContext {
    std::uint64_t user_rsp;
    std::uint64_t kernel_rsp;
};

void initSyscall();
void setSyscallKernelStack(std::uint64_t rsp);

extern SyscallContext g_syscall_context;

} // namespace oz
