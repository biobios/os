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

// For process/thread management (Phase 3)
// Creates a new thread with kernel stack, returns Thread*
extern Thread* (*g_create_thread_func)(void (*entry)(void*), void* arg);
// Queues a thread into the scheduler
extern void (*g_queue_thread_func)(Thread* t);
// Creates a new VSpace (cloned from kernel space), returns opaque ptr + map/unmap fns
struct VSpaceCreateResult {
    void* address_space;  // ProcessAddressSpace*
    void* pml4_phys;      // Physical address of PML4
};
extern VSpaceCreateResult (*g_create_vspace_func)(UntypedMemory* untyped);
// Global VSpace map/unmap function pointers (set by Kernel.hpp, used by Syscall.cpp)
extern bool (*g_vspace_map_fn)(void*, std::uintptr_t, std::uintptr_t, std::uint64_t);
extern bool (*g_vspace_unmap_fn)(void*, std::uintptr_t);

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
