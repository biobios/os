#include "syscall.hpp"
#include "loader.hpp"
#include "../common/test_harness.hpp"
#include "../../src/common/ipc/Protocol.hpp"
#include "../../src/common/utils/BootInfo.hpp"

namespace oz::stest {

static constexpr std::uint64_t BOOTINFO_VADDR = 0x600000000000ULL;
static constexpr std::uint64_t INITRD_VADDR   = 0x500000000000ULL;

// =============================================================================
// 1. Radix-64 Multi-stage Notification Tests
// =============================================================================
static void run_radix64_notification_tests() {
    oz::sys_debug_print("\n\r--- 1. Radix-64 Multi-stage Notification Tests ---\n\r");

    // 1.1 One-Hot Badge Invariant
    std::uint32_t root_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t child_notif = oz::sys_notification_create(oz::CAP_UNTYPED);

    int b_res = oz::sys_notification_bind_parent(child_notif, root_notif, 0);
    TEST_REPORT("STEST", "One-hot badge invariant (badge=0 rejected)", b_res == oz::SYS_ERR_INVALID);

    b_res = oz::sys_notification_bind_parent(child_notif, root_notif, 3);
    TEST_REPORT("STEST", "One-hot badge invariant (badge=3 rejected)", b_res == oz::SYS_ERR_INVALID);

    b_res = oz::sys_notification_bind_parent(child_notif, root_notif, 1ULL << 1);
    TEST_REPORT("STEST", "Binding valid child with one-hot badge", b_res == 0);

    // 1.2 Acyclic Invariant (Cycle Rejection)
    int cyc_self = oz::sys_notification_bind_parent(root_notif, root_notif, 1ULL << 0);
    TEST_REPORT("STEST", "Acyclic invariant (self-binding returned SYS_ERR_DEADLK)", cyc_self == oz::SYS_ERR_DEADLK);

    int cyc_direct = oz::sys_notification_bind_parent(root_notif, child_notif, 1ULL << 2);
    TEST_REPORT("STEST", "Acyclic invariant (direct cycle A->B->A returned SYS_ERR_DEADLK)", cyc_direct == oz::SYS_ERR_DEADLK);

    std::uint32_t gc_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    oz::sys_notification_bind_parent(gc_notif, child_notif, 1ULL << 0);
    int cyc_indirect = oz::sys_notification_bind_parent(root_notif, gc_notif, 1ULL << 0);
    TEST_REPORT("STEST", "Acyclic invariant (indirect cycle A->B->C->A returned SYS_ERR_DEADLK)", cyc_indirect == oz::SYS_ERR_DEADLK);

    // 1.3 Max Depth Limit (Max 4 levels)
    std::uint32_t l4_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    int d4_res = oz::sys_notification_bind_parent(l4_notif, gc_notif, 1ULL << 0);
    TEST_REPORT("STEST", "Binding Level 4 notification succeeded", d4_res == 0);

    std::uint32_t l5_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    int d5_res = oz::sys_notification_bind_parent(l5_notif, l4_notif, 1ULL << 0);
    TEST_REPORT("STEST", "Max Depth limit (depth 5 rejected with SYS_ERR_INVALID)", d5_res == oz::SYS_ERR_INVALID);

    // 1.4 Unbinding
    int unbind_res = oz::sys_notification_unbind_parent(l4_notif);
    TEST_REPORT("STEST", "sys_notification_unbind_parent succeeded", unbind_res == 0);

    // 1.5 Edge-Triggered Cascading & Shortcut Optimization
    std::uint32_t edge_p = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t edge_c = oz::sys_notification_create(oz::CAP_UNTYPED);
    oz::sys_notification_bind_parent(edge_c, edge_p, 1ULL << 10);

    oz::sys_notification_signal(edge_c, 1ULL << 2);
    std::uint64_t p_val = 0;
    oz::sys_notification_poll(edge_p, &p_val);
    TEST_REPORT("STEST", "Edge-triggered cascading (0 -> non-zero signalled parent)", p_val == (1ULL << 10));

    // Shortcut: already non-zero
    oz::sys_notification_signal(edge_c, 1ULL << 3);
    p_val = 0;
    oz::sys_notification_poll(edge_p, &p_val);
    TEST_REPORT("STEST", "Shortcut optimization (no cascade when child already non-zero)", p_val == 0);

    // Poll clear and signal again
    std::uint64_t c_val = 0;
    oz::sys_notification_poll(edge_c, &c_val);
    oz::sys_notification_signal(edge_c, 1ULL << 4);
    p_val = 0;
    oz::sys_notification_poll(edge_p, &p_val);
    TEST_REPORT("STEST", "Rising edge after clear cascaded to parent again", p_val == (1ULL << 10));

    // Cleanup
    oz::sys_cap_delete(root_notif);
    oz::sys_cap_delete(child_notif);
    oz::sys_cap_delete(gc_notif);
    oz::sys_cap_delete(l4_notif);
    oz::sys_cap_delete(l5_notif);
    oz::sys_cap_delete(edge_p);
    oz::sys_cap_delete(edge_c);
}

// =============================================================================
// 2. User-space Scheduler Daemon Integration Tests
// =============================================================================
static void run_sched_daemon_tests(const std::uint8_t* initrd_ptr, std::uint64_t initrd_size) {
    oz::sys_debug_print("\n\r--- 2. Scheduler Daemon Integration Tests ---\n\r");

    oz::loader::TarFile sched_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "sched");
    if (!sched_file.data) {
        TEST_REPORT("STEST", "Find sched binary in initrd", false);
        return;
    }
    TEST_REPORT("STEST", "Find sched binary in initrd", true);

    auto sched_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, sched_file.data, sched_file.size);
    if (!sched_proc.success) {
        TEST_REPORT("STEST", "Spawn sched daemon process", false);
        return;
    }
    TEST_REPORT("STEST", "Spawn sched daemon process", true);

    std::uint32_t sched_send = 0;
    std::uint32_t sched_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &sched_send, &sched_recv);
    std::uint32_t sched_notif = oz::sys_notification_create(oz::CAP_UNTYPED);

    static constexpr std::uint64_t BADGE_SCHED_IPC = 1ULL << 63;
    oz::sys_channel_bind_notification(sched_recv, sched_notif, BADGE_SCHED_IPC);

    // Slot 2: sched IPC recv, Slot 3: sched notif, Slot 240: untyped
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, 2, sched_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, 3, sched_notif, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));

    std::uint32_t sched_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_tcb_bind_baton(sched_proc.tcb_cap, sched_baton);
    oz::sys_baton_inject(sched_baton, 100000);
    oz::sys_tcb_resume(sched_proc.tcb_cap);
    oz::sys_baton_switch(sched_baton);

    // Test thread registration IPC
    std::uint32_t dummy_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    constexpr std::uint32_t COPY_SLOT = 500;
    oz::sys_cap_copy(COPY_SLOT, dummy_tcb, static_cast<std::uint32_t>(oz::CapRights::ALL));

    oz::IPCMessage reg_msg{};
    reg_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::SchedCmd::REGISTER_THREAD);
    reg_msg.data[1] = 2; // Priority
    reg_msg.transferred_cap = COPY_SLOT;
    reg_msg.has_cap = true;

    oz::IPCMessage reg_resp{};
    bool reg_ok = oz::sys_call(sched_send, &reg_msg, &reg_resp) && (reg_resp.data[0] == 0);
    TEST_REPORT("STEST", "Register thread with Sched Daemon (IPC REGISTER_THREAD)", reg_ok);
}

} // namespace oz::stest

extern "C" void _start() {
    oz::sys_debug_print("\n\r========================================================\n\r");
    oz::sys_debug_print(" [OZ_OS] Scheduler & Notification Test Runner (stest)\n\r");
    oz::sys_debug_print("========================================================\n\r");

    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_BOOTINFO, oz::stest::BOOTINFO_VADDR, 0x00, 4096) != 0) {
        oz::test::report("STEST", "Map BootInfo", false);
        oz::test::exit_qemu();
    }
    const volatile auto* boot_info = reinterpret_cast<const volatile oz::BootInfo*>(oz::stest::BOOTINFO_VADDR);
    std::uint64_t initrd_size = boot_info->initrd_size;

    if (initrd_size > 0) {
        oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_INITRD, oz::stest::INITRD_VADDR, 0x00, initrd_size);
    }
    const std::uint8_t* initrd_ptr = reinterpret_cast<const std::uint8_t*>(oz::stest::INITRD_VADDR + boot_info->initrd_offset);

    oz::stest::run_radix64_notification_tests();
    oz::stest::run_sched_daemon_tests(initrd_ptr, initrd_size);

    oz::test::print_summary("STEST");
    oz::test::exit_qemu();
}
