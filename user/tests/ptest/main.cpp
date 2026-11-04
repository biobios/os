#include "syscall.hpp"
#include "loader.hpp"
#include "../common/test_harness.hpp"
#include "../../src/common/ipc/Protocol.hpp"
#include "../../src/common/ipc/StartupInfo.hpp"
#include "../../src/common/utils/BootInfo.hpp"

namespace oz::ptest {

static constexpr std::uint64_t BOOTINFO_VADDR = 0x600000000000ULL;
static constexpr std::uint64_t INITRD_VADDR   = 0x500000000000ULL;

} // namespace oz::ptest

extern "C" void _start() {
    oz::sys_debug_print("\n\r========================================================\n\r");
    oz::sys_debug_print(" [OZ_OS] Process & Memory Manager Test Runner (ptest)\n\r");
    oz::sys_debug_print("========================================================\n\r");

    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_BOOTINFO, oz::ptest::BOOTINFO_VADDR, 0x00, 4096) != 0) {
        oz::test::report("PTEST", "Map BootInfo", false);
        oz::test::exit_qemu();
    }
    const volatile auto* boot_info = reinterpret_cast<const volatile oz::BootInfo*>(oz::ptest::BOOTINFO_VADDR);
    std::uint64_t initrd_size = boot_info->initrd_size;

    if (initrd_size > 0) {
        oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_INITRD, oz::ptest::INITRD_VADDR, 0x00, initrd_size);
    }
    const std::uint8_t* initrd_ptr = reinterpret_cast<const std::uint8_t*>(oz::ptest::INITRD_VADDR + boot_info->initrd_offset);

    std::uint32_t init_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    auto init_coroutine = [&](std::uint32_t tcb_cap) {
        if (tcb_cap == oz::INVALID_CAP_ID) return;
        oz::sys_tcb_bind_baton(tcb_cap, init_baton);
        oz::sys_baton_inject(init_baton, 1000000);
        oz::sys_baton_switch(init_baton);
    };

    // Dummy console endpoints
    std::uint32_t cons_send = 0, cons_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &cons_send, &cons_recv);

    // =========================================================================
    // 1. MemMan (Memory Manager) Tests
    // =========================================================================
    oz::sys_debug_print("\n\r--- 1. MemMan Service Tests ---\n\r");

    oz::loader::TarFile mem_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "memman");
    TEST_REPORT("PTEST", "Find memman binary in initrd", mem_file.data != nullptr);
    if (!mem_file.data) {
        oz::test::print_summary("PTEST");
        oz::test::exit_qemu();
    }

    auto mem_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, mem_file.data, mem_file.size);
    TEST_REPORT("PTEST", "Spawn memman daemon process", mem_proc.success);

    std::uint32_t mem_req_send = 0, mem_req_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &mem_req_send, &mem_req_recv);

    std::uint32_t mem_reply_send = 0, mem_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &mem_reply_send, &mem_reply_recv);

    oz::sys_cspace_cap_insert(mem_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(mem_proc.cspace_cap, 2, mem_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::GRANT_REPLY));
    oz::sys_cspace_cap_insert(mem_proc.cspace_cap, 3, mem_reply_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    std::uint32_t mem_ut = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory), 16 * 1024 * 1024);
    oz::sys_cspace_cap_move(mem_proc.cspace_cap, oz::CAP_UNTYPED, mem_ut);

    oz::sys_tcb_resume(mem_proc.tcb_cap);
    init_coroutine(mem_proc.tcb_cap);

    // Test 1.1: MemCmd::GET_INFO
    oz::IPCMessage info_req{};
    info_req.data[0] = static_cast<std::uint64_t>(oz::ipc::MemCmd::GET_INFO);
    oz::IPCMessage info_resp{};
    bool info_ok = oz::sys_call(mem_req_send, &info_req, &info_resp) && (info_resp.data[0] == 0);
    TEST_REPORT("PTEST", "MemMan GET_INFO IPC query", info_ok);

    // Test 1.2: MemCmd::ALLOC_UNTYPED
    oz::IPCMessage alloc_req{};
    alloc_req.data[0] = static_cast<std::uint64_t>(oz::ipc::MemCmd::ALLOC_UNTYPED);
    alloc_req.data[1] = 2 * 1024 * 1024; // 2MB
    oz::IPCMessage alloc_resp{};
    bool alloc_ok = oz::sys_call_recv_cap(mem_req_send, &alloc_req, &alloc_resp) && (alloc_resp.data[0] == 0) && alloc_resp.has_cap;
    TEST_REPORT("PTEST", "MemMan ALLOC_UNTYPED cap transfer", alloc_ok && alloc_resp.transferred_cap != oz::INVALID_CAP_ID);

    std::uint32_t child_untyped = alloc_resp.transferred_cap;

    // Test 1.3: MemCmd::REVOKE_UNTYPED
    if (alloc_ok) {
        oz::IPCMessage rev_req{};
        rev_req.data[0] = static_cast<std::uint64_t>(oz::ipc::MemCmd::REVOKE_UNTYPED);
        rev_req.has_cap = true;
        rev_req.transferred_cap = child_untyped;
        oz::IPCMessage rev_resp{};
        bool rev_ok = oz::sys_call(mem_req_send, &rev_req, &rev_resp) && (rev_resp.data[0] == 0);
        TEST_REPORT("PTEST", "MemMan REVOKE_UNTYPED cap reclaim", rev_ok);
    }

    // =========================================================================
    // 2. ProcMan (Process Manager) Tests
    // =========================================================================
    oz::sys_debug_print("\n\r--- 2. ProcMan Service Tests ---\n\r");

    oz::loader::TarFile proc_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "procman");
    TEST_REPORT("PTEST", "Find procman binary in initrd", proc_file.data != nullptr);
    if (proc_file.data) {
        auto procman_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, proc_file.data, proc_file.size);
        TEST_REPORT("PTEST", "Spawn procman daemon process", procman_proc.success);

        if (procman_proc.success) {
            std::uint32_t proc_req_send = 0, proc_req_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &proc_req_send, &proc_req_recv);

            std::uint32_t proc_reply_send = 0, proc_reply_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &proc_reply_send, &proc_reply_recv);

            std::uint32_t proc_mem_send = 0, proc_mem_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &proc_mem_send, &proc_mem_recv);

            std::uint32_t dummy_vfs_send = 0, dummy_vfs_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &dummy_vfs_send, &dummy_vfs_recv);

            std::uint32_t dummy_shared_buf = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);

            // ProcMan CSpace layout:
            // 1: Console, 2: ProcReq Recv, 3: ProcReply Send, 4: MemSend, 5: MemRecv, 6: VFSSend, 7: VFSRecv, 8: VFS SharedBuf
            oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
            oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 2, proc_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::GRANT_REPLY));
            oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 3, proc_reply_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
            oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 4, mem_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
            oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 5, mem_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
            oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 6, dummy_vfs_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
            oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 7, dummy_vfs_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
            oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 8, dummy_shared_buf, static_cast<std::uint32_t>(oz::CapRights::ALL));

            oz::sys_tcb_resume(procman_proc.tcb_cap);
            init_coroutine(procman_proc.tcb_cap);

            // Test 2.1: ProcCmd::CREATE_PIPE
            oz::IPCMessage pipe_req{};
            pipe_req.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::CREATE_PIPE);
            oz::IPCMessage pipe_resp{};
            bool pipe_ok = oz::sys_call_recv_cap(proc_req_send, &pipe_req, &pipe_resp) &&
                           (pipe_resp.data[0] == 0) && pipe_resp.has_cap;
            TEST_REPORT("PTEST", "ProcMan CREATE_PIPE IPC", pipe_ok && pipe_resp.transferred_cap != oz::INVALID_CAP_ID);

            if (pipe_ok && pipe_resp.transferred_cap != oz::INVALID_CAP_ID) {
                oz::sys_cap_delete(pipe_resp.transferred_cap);
            }
        }
    }

    oz::test::print_summary("PTEST");
    oz::test::exit_qemu();
}
