#include "syscall.hpp"
#include "../common/test_harness.hpp"
static void print_hex(std::uint64_t val) {
    char buf[17];
    for (int i = 15; i >= 0; --i) {
        int nibble = (val >> (i * 4)) & 0xF;
        buf[15 - i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    buf[16] = '\0';
    oz::sys_debug_print(buf);
}

namespace oz::ktest {

// =============================================================================
// 1. Capability Semantics Tests
// =============================================================================
static void run_capability_semantics_tests() {
    oz::sys_debug_print("\n\r--- 1. Capability Semantics Tests ---\n\r");

    // Test Move semantics
    std::uint32_t t_ep0 = 0, t_ep1 = 0;
    if (oz::sys_channel_create(oz::CAP_UNTYPED, &t_ep0, &t_ep1)) {
        if (oz::sys_cap_move(50, t_ep0) == 0) {
            bool source_invalidated = (oz::sys_cap_delete(t_ep0) != 0);
            bool dest_deleted = (oz::sys_cap_delete(50) == 0);
            TEST_REPORT("KTEST", "Capability Move semantics", source_invalidated && dest_deleted);
        } else {
            TEST_REPORT("KTEST", "Capability Move semantics", false);
        }
        oz::sys_cap_delete(t_ep1);
    } else {
        TEST_REPORT("KTEST", "Capability Move semantics (channel create)", false);
    }

    // Test Linear rejection on copy/mint
    std::uint32_t minted_linear = oz::sys_cap_mint(oz::CAP_ROOT_IOPORT, static_cast<std::uint32_t>(oz::CapRights::ALL));
    if (minted_linear == oz::INVALID_CAP_ID) {
        TEST_REPORT("KTEST", "Linear duplication prevention", true);
    } else {
        TEST_REPORT("KTEST", "Linear duplication prevention", false);
        oz::sys_cap_delete(minted_linear);
    }

    // Test C++ RAII LinearCap
    {
        std::uint32_t r_ep0 = 0, r_ep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &r_ep0, &r_ep1)) {
            oz::LinearCap<> linear1(r_ep0);
            oz::LinearCap<> linear2 = static_cast<oz::LinearCap<>&&>(linear1);
            if (!linear1.isValid() && linear2.isValid()) {
                bool moved = linear2.moveTo(51);
                TEST_REPORT("KTEST", "C++ LinearCap RAII move", moved);
            } else {
                TEST_REPORT("KTEST", "C++ LinearCap RAII move", false);
            }
            oz::sys_cap_delete(r_ep1);
        }
    }
    // linear2 went out of scope, slot 51 should be deleted automatically
    TEST_REPORT("KTEST", "C++ LinearCap RAII destructor cleanup", oz::sys_cap_delete(51) != 0);

    // Test ScopedCap reset & double free prevention
    {
        std::uint32_t r_ep0 = 0, r_ep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &r_ep0, &r_ep1)) {
            oz::ScopedCap<> scoped(r_ep0);
            scoped.reset();
            TEST_REPORT("KTEST", "ScopedCap reset idempotency & double free prevention", !scoped.isValid() && oz::sys_cap_delete(r_ep0) != 0);
            oz::sys_cap_delete(r_ep1);
        }
    }

    // Test Notification Object & Bound Notification
    std::uint32_t notif_cap = oz::sys_notification_create(oz::CAP_UNTYPED);
    if (notif_cap != oz::INVALID_CAP_ID) {
        TEST_REPORT("KTEST", "Notification creation", true);

        // Test manual signal and poll
        oz::sys_notification_signal(notif_cap, 0x42);
        std::uint64_t polled_bits = 0;
        bool sig_poll_ok = (oz::sys_notification_poll(notif_cap, &polled_bits) == 0 && polled_bits == 0x42);
        TEST_REPORT("KTEST", "Notification signal & poll", sig_poll_ok);

        // Test Bound Notification with Channel
        std::uint32_t b_ep0 = 0, b_ep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &b_ep0, &b_ep1)) {
            if (oz::sys_channel_bind_notification(b_ep1, notif_cap, 0x100) == 0) {
                oz::sys_cap_delete(b_ep0);

                std::uint64_t close_bits = 0;
                bool client_disc_ok = (oz::sys_notification_poll(notif_cap, &close_bits) == 0 && (close_bits & 0x100));
                TEST_REPORT("KTEST", "Bound Notification on client disconnect", client_disc_ok);
            } else {
                TEST_REPORT("KTEST", "Bound Notification binding", false);
            }
            oz::sys_cap_delete(b_ep1);
        }
        oz::sys_cap_delete(notif_cap);
    } else {
        TEST_REPORT("KTEST", "Notification creation", false);
    }

    // Test Rights and Boundary Checks
    {
        // 1. Delete on invalid slot must fail
        bool del_inv = (oz::sys_cap_delete(99999) != 0);
        TEST_REPORT("KTEST", "sys_cap_delete on non-existent slot rejected", del_inv);

        // 2. Privilege escalation prevention on mint
        std::uint32_t ro_ep0 = 0, ro_ep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &ro_ep0, &ro_ep1)) {
            std::uint32_t ro_cap = oz::sys_cap_mint(ro_ep0, static_cast<std::uint32_t>(oz::CapRights::READ));
            if (ro_cap != oz::INVALID_CAP_ID) {
                std::uint32_t escalated = oz::sys_cap_mint(ro_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
                TEST_REPORT("KTEST", "Cap privilege escalation rejected on mint", escalated == oz::INVALID_CAP_ID);
                if (escalated != oz::INVALID_CAP_ID) {
                    oz::sys_cap_delete(escalated);
                }
                oz::IPCMessage bad_msg{};
                oz::IPCMessage bad_rep{};
                bool call_fail = !oz::sys_call(ro_cap, &bad_msg, &bad_rep);
                TEST_REPORT("KTEST", "sys_call rejected on READ-only cap", call_fail);

                oz::sys_cap_delete(ro_cap);
            }
            oz::sys_cap_delete(ro_ep0);
            oz::sys_cap_delete(ro_ep1);
        }

        // 3. sys_cap_revoke preserves caller's slot while revoking copies
        std::uint32_t orig_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
        if (orig_notif != oz::INVALID_CAP_ID) {
            std::uint32_t copied_notif = oz::sys_cap_mint(orig_notif, static_cast<std::uint32_t>(oz::CapRights::ALL));
            if (copied_notif != oz::INVALID_CAP_ID) {
                int revoke_res = oz::sys_cap_revoke(orig_notif);

                // orig_notif should still be valid
                int sig_res = oz::sys_notification_signal(orig_notif, 0xAA);
                std::uint64_t val = 0;
                int poll_res = oz::sys_notification_poll(orig_notif, &val);
                bool orig_valid = (revoke_res == 0 && sig_res == 0 && poll_res == 0 && val == 0xAA);

                // copied_notif should be invalidated
                bool copied_invalid = (oz::sys_notification_signal(copied_notif, 0xBB) != 0) &&
                                      (oz::sys_cap_delete(copied_notif) != 0);

                // orig_notif deletion should succeed
                bool orig_del_ok = (oz::sys_cap_delete(orig_notif) == 0);

                TEST_REPORT("KTEST", "sys_cap_revoke preserves caller and revokes copies",
                            orig_valid && copied_invalid && orig_del_ok);
            } else {
                TEST_REPORT("KTEST", "sys_cap_revoke preserves caller and revokes copies", false);
                oz::sys_cap_delete(orig_notif);
            }
        } else {
            TEST_REPORT("KTEST", "sys_cap_revoke preserves caller and revokes copies", false);
        }
    }
}

// =============================================================================
// 2. Baton Scheduling Context Tests
// =============================================================================
static volatile bool g_rebind_t1_entered = false;
static volatile bool g_rebind_t2_entered = false;
static volatile bool g_rebind_t1_resumed = false;
static std::uint32_t g_rebind_t1_tcb = 0;
static std::uint32_t g_rebind_t2_tcb = 0;
static std::uint32_t g_rebind_shared_baton = 0;

static void test_rebind_t1_thread() {
    g_rebind_t1_entered = true;
    oz::sys_tcb_bind_baton(g_rebind_t2_tcb, g_rebind_shared_baton);
    g_rebind_t1_resumed = true;
    oz::sys_yield();
    oz::sys_exit(0);
}

static void test_rebind_t2_thread() {
    g_rebind_t2_entered = true;
    oz::sys_tcb_bind_baton(g_rebind_t1_tcb, g_rebind_shared_baton);
    oz::sys_exit(0);
}

static volatile bool g_zero_worker_ran = false;
static void test_zero_worker_thread() {
    g_zero_worker_ran = true;
    oz::sys_yield();
    oz::sys_exit(0);
}

static volatile bool g_abort_worker_ran = false;
static volatile int g_abort_result = 0;
static volatile std::uint32_t g_abort_ep = 0;
static void test_abort_worker_thread() {
    g_abort_worker_ran = true;
    oz::IPCMessage msg{};
    std::int64_t ret = static_cast<std::int64_t>(
        oz::sys_reply_and_recv(g_abort_ep, nullptr, &msg));
    g_abort_result = static_cast<int>(ret);
    oz::sys_exit(0);
}

static volatile bool g_server_first_recv_ok = false;
static volatile std::uint32_t g_dead_client_srv_ep = 0;
static volatile std::uint32_t g_dead_client_cli_ep = 0;
static void test_orphan_server_thread() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    oz::ReplyToken tok = oz::sys_reply_recv(oz::INVALID_REPLY_TOKEN, g_dead_client_srv_ep, nullptr, &req);
    g_server_first_recv_ok = (tok != oz::INVALID_REPLY_TOKEN);
    oz::sys_reply_recv(0xDEADBEEF, g_dead_client_srv_ep, &rep, &req);
    oz::sys_exit(0);
}

static void test_orphan_client_thread() {
    oz::IPCMessage send_msg{};
    oz::IPCMessage recv_msg{};
    send_msg.data[0] = 0x1234;
    oz::sys_call(g_dead_client_cli_ep, &send_msg, &recv_msg);
    oz::sys_exit(0);
}

static volatile bool g_pf_resolved = false;
static volatile std::uint64_t g_pf_fault_addr = 0;
static volatile bool g_pf_client_success = false;
static std::uint32_t g_pf_pager_ep = 0;
static std::uint32_t g_pf_frame_cap = 0;
static constexpr std::uint64_t FAULT_TEST_VADDR = 0x650000000000ULL;

static void test_pf_pager_thread() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    oz::ReplyToken tok = oz::sys_reply_recv(oz::INVALID_REPLY_TOKEN, g_pf_pager_ep, nullptr, &req);
    g_pf_fault_addr = req.data[0];

    std::uint64_t map_vaddr = g_pf_fault_addr & ~0xFFFULL;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, g_pf_frame_cap, map_vaddr, 0x07, 4096);
    g_pf_resolved = true;

    oz::sys_reply_recv(tok, g_pf_pager_ep, &rep, &req);
    oz::sys_exit(0);
}

static void test_pf_client_thread() {
    volatile std::uint32_t* ptr = reinterpret_cast<volatile std::uint32_t*>(FAULT_TEST_VADDR);
    *ptr = 0xCAFEBABE;
    if (*ptr == 0xCAFEBABE) {
        g_pf_client_success = true;
    }
    oz::sys_yield();
    oz::sys_exit(0);
}

static volatile int g_irq_replenish_step = 0;
static std::uint32_t g_test_irq_handler_cap = 0;
static std::uint32_t g_test_irq_baton_a = 0;
static std::uint32_t g_test_irq_baton_b = 0;

static void test_irq_replenish_handler_entry() {
    g_irq_replenish_step = 1;
    oz::sys_irq_wait(g_test_irq_handler_cap, g_test_irq_baton_a);
    g_irq_replenish_step = 2;
    oz::sys_irq_wait(g_test_irq_handler_cap, g_test_irq_baton_b);
    g_irq_replenish_step = 3;
    oz::sys_exit(0);
}

// Helpers for Dual Baton & Nonblocking IPC Tests
static volatile bool g_forced_block_ran = false;
static volatile std::uint32_t g_forced_block_ep = 0;
static void test_forced_block_worker() {
    g_forced_block_ran = true;
    oz::IPCMessage msg{};
    oz::sys_reply_and_recv(g_forced_block_ep, nullptr, &msg, oz::IPC_FLAG_BLOCK);
    oz::sys_exit(0);
}

static volatile int g_safety_guard_exit_res = 0;
static volatile std::uint32_t g_safety_srv_ep = 0;
static void test_safety_guard_worker() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    int res = oz::sys_reply_and_recv(g_safety_srv_ep, nullptr, &req, oz::IPC_FLAG_BLOCK);
    if (res == 0) {
        std::int64_t exit_ret = oz::syscall(60, 0, 0, 0, 0, 0, 0);
        g_safety_guard_exit_res = static_cast<int>(exit_ret);
        rep.data[0] = 0x7777;
        oz::sys_reply_and_recv(g_safety_srv_ep, &rep, nullptr, 0);
    }
    oz::sys_exit(0);
}

static volatile bool g_safety_client_received_reply = false;
static volatile std::uint32_t g_safety_cli_ep = 0;
static void test_safety_guard_client() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    req.data[0] = 0x1111;
    if (oz::sys_call(g_safety_cli_ep, &req, &rep) && rep.data[0] == 0x7777) {
        g_safety_client_received_reply = true;
    }
    oz::sys_exit(0);
}

static volatile bool g_nb_worker_ran = false;
static volatile std::uint32_t g_nb_srv_ep = 0;
static void test_nb_rpc_worker() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    int res = oz::sys_reply_and_recv(g_nb_srv_ep, nullptr, &req, oz::IPC_FLAG_BLOCK);
    if (res == 0) {
        rep.data[0] = req.data[0] + 1;
        int rep_res = oz::sys_reply_and_recv(g_nb_srv_ep, &rep, &req, 0);
        if (rep_res == oz::SYS_ERR_WOULDBLOCK) {
            g_nb_worker_ran = true;
        }
    }
    oz::sys_exit(0);
}

static volatile bool g_nb_client_reply_ok = false;
static volatile std::uint32_t g_nb_cli_ep = 0;
static void test_nb_rpc_client() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    req.data[0] = 41;
    if (oz::sys_call(g_nb_cli_ep, &req, &rep) && rep.data[0] == 42) {
        g_nb_client_reply_ok = true;
    }
    oz::sys_exit(0);
}

static volatile bool g_dead_cli_srv_ran = false;
static volatile std::uint32_t g_dead_cli_srv_ep = 0;
static void test_dead_client_nb_server() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    int res = oz::sys_reply_and_recv(g_dead_cli_srv_ep, nullptr, &req, oz::IPC_FLAG_BLOCK);
    if (res == 0) {
        oz::sys_yield();
        int res2 = oz::sys_reply_and_recv(g_dead_cli_srv_ep, &rep, &req, 0);
        if (res2 == oz::SYS_ERR_WOULDBLOCK) {
            g_dead_cli_srv_ran = true;
        }
    }
    oz::sys_exit(0);
}

static volatile std::uint32_t g_dead_cli_ep = 0;
static void test_dead_client_call() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    req.data[0] = 0x9999;
    oz::sys_call(g_dead_cli_ep, &req, &rep);
    oz::sys_exit(0);
}

static volatile bool g_call_first_cli_ok = false;
static volatile std::uint32_t g_call_first_cli_ep = 0;
static void test_call_first_client() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    req.data[0] = 100;
    if (oz::sys_call(g_call_first_cli_ep, &req, &rep) && rep.data[0] == 200) {
        g_call_first_cli_ok = true;
    }
    oz::sys_exit(0);
}

static volatile bool g_call_first_srv_ran = false;
static volatile std::uint32_t g_call_first_srv_ep = 0;
static void test_call_first_server() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    int res = oz::sys_reply_and_recv(g_call_first_srv_ep, nullptr, &req, 0);
    if (res == 0) {
        g_call_first_srv_ran = true;
        rep.data[0] = req.data[0] + 100;
        oz::sys_reply_and_recv(g_call_first_srv_ep, &rep, nullptr, 0);
    }
    oz::sys_exit(0);
}

static volatile std::uint32_t g_nested_srv_c_ep = 0;
static volatile std::uint32_t g_nested_call_c_ep = 0;
static volatile std::uint32_t g_nested_srv_b_ep = 0;
static volatile std::uint32_t g_nested_call_b_ep = 0;
static void test_nested_server_c() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    int res = oz::sys_reply_and_recv(g_nested_srv_c_ep, nullptr, &req, oz::IPC_FLAG_BLOCK);
    if (res == 0) {
        rep.data[0] = req.data[0] * 10;
        oz::sys_reply_and_recv(g_nested_srv_c_ep, &rep, nullptr, 0);
    }
    oz::sys_exit(0);
}

static void test_nested_server_b() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    int res = oz::sys_reply_and_recv(g_nested_srv_b_ep, nullptr, &req, oz::IPC_FLAG_BLOCK);
    if (res == 0) {
        oz::IPCMessage c_req{};
        oz::IPCMessage c_rep{};
        c_req.data[0] = 20;
        oz::sys_call(g_nested_call_c_ep, &c_req, &c_rep);
        rep.data[0] = req.data[0] + c_rep.data[0];
        oz::sys_reply_and_recv(g_nested_srv_b_ep, &rep, nullptr, 0);
    }
    oz::sys_exit(0);
}

static volatile std::uint64_t g_nested_client_res = 0;
static void test_nested_client_a() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    req.data[0] = 10;
    if (oz::sys_call(g_nested_call_b_ep, &req, &rep)) {
        g_nested_client_res = rep.data[0];
    }
    oz::sys_exit(0);
}

// -----------------------------------------------------------------------------
// Hierarchical Cartesian Scheduling Test Helpers
// -----------------------------------------------------------------------------
static volatile int g_h_explicit_counter = 0;
static void test_h_explicit_worker() {
    g_h_explicit_counter = 1;
    oz::sys_baton_yield();
    g_h_explicit_counter = 2;
    oz::sys_baton_exit(0);
}

static volatile int g_h_deadlock_res = 0;
static std::uint32_t g_h_parent_baton = 0;
static void test_h_deadlock_child() {
    oz::SchedResult res = oz::sys_baton_switch(g_h_parent_baton, 10000);
    g_h_deadlock_res = static_cast<int>(res.reason);
    oz::sys_baton_exit(0);
}
static void test_h_deadlock_parent() {
    std::uint32_t c_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t c_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t c_stack_top = 0x7000d8000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, c_stack, 0x7000d8000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(c_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_h_deadlock_child), c_stack_top);
    std::uint32_t c_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_tcb_bind_baton(c_tcb, c_baton);

    oz::sys_baton_switch(c_baton, 50000);
    oz::sys_baton_exit(0);
}

static volatile bool g_h_worker_started = false;
static volatile bool g_h_timeout_occurred = false;
static volatile bool g_h_worker_resumed = false;
static volatile bool g_h_subsched_finished = false;
static std::uint32_t g_h_worker_baton = 0;

static void test_h_worker_thread() {
    g_h_worker_started = true;
    while (!g_h_timeout_occurred) {
        __asm__ volatile("pause");
    }
    g_h_worker_resumed = true;
    oz::sys_baton_exit(0);
}

static void test_h_subsched_thread() {
    oz::sys_baton_switch(g_h_worker_baton, 200000);
    g_h_subsched_finished = true;
    oz::sys_baton_exit(0);
}

static void test_h_crash_worker() {
    *reinterpret_cast<volatile int*>(0xdead0000ULL) = 42;
    oz::sys_baton_exit(0);
}

static void run_baton_scheduling_tests() {
    oz::sys_debug_print("\n\r--- 2. Baton Scheduling Context Tests ---\n\r");

    // 1. Rebind-and-Switch cooperative thread switching
    g_rebind_shared_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(g_rebind_shared_baton, 100000);

    g_rebind_t1_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t t1_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t t1_stack_top = 0x700000000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, t1_stack, 0x700000000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(g_rebind_t1_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_rebind_t1_thread), t1_stack_top);

    g_rebind_t2_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t t2_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t t2_stack_top = 0x700008000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, t2_stack, 0x700008000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(g_rebind_t2_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_rebind_t2_thread), t2_stack_top);

    g_rebind_t1_entered = false;
    g_rebind_t2_entered = false;
    g_rebind_t1_resumed = false;

    oz::sys_tcb_bind_baton(g_rebind_t1_tcb, g_rebind_shared_baton);
    oz::SchedResult res = oz::sys_baton_switch(g_rebind_shared_baton);
    bool rebind_ok = (g_rebind_t1_entered && g_rebind_t2_entered && g_rebind_t1_resumed && res.reason == oz::SchedReason::YIELD);
    TEST_REPORT("KTEST", "Rebind-and-Switch cooperative thread switching", rebind_ok);
    oz::sys_baton_switch(g_rebind_shared_baton);

    // 2. Test budget == 0 Notification Wakeup + sys_baton_inject Replenishment
    std::uint32_t zero_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t zero_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    static constexpr std::uint64_t ZERO_BADGE = (1ULL << 8);
    oz::sys_baton_bind_notification(zero_baton, zero_notif, ZERO_BADGE);

    std::uint32_t zero_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t zero_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t zero_stack_top = 0x700010000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, zero_stack, 0x700010000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(zero_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_zero_worker_thread), zero_stack_top);

    oz::sys_tcb_bind_baton(zero_tcb, zero_baton);
    oz::SchedResult zres = oz::sys_baton_switch(zero_baton);

    std::uint64_t polled_badge = 0;
    bool notif_received = (oz::sys_notification_poll(zero_notif, &polled_badge) == 0 && (polled_badge & ZERO_BADGE));
    bool budget_wakeup_ok = (!g_zero_worker_ran && zres.reason == oz::SYS_BATON_SWITCH_TIMEOUT && notif_received);
    TEST_REPORT("KTEST", "budget == 0 Notification wakeup", budget_wakeup_ok);

    oz::sys_baton_inject(zero_baton, 50000);
    zres = oz::sys_baton_switch(zero_baton);
    bool replenish_ok = (g_zero_worker_ran && zres.reason == oz::SchedReason::YIELD);
    TEST_REPORT("KTEST", "sys_baton_inject replenishment and execution", replenish_ok);
    oz::sys_baton_switch(zero_baton);

    // 3. Test Aborting IPC on Baton Bind
    std::uint32_t abort_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t abort_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t abort_stack_top = 0x700020000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, abort_stack, 0x700020000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(abort_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_abort_worker_thread), abort_stack_top);

    std::uint32_t ep_a0 = oz::INVALID_CAP_ID, ep_a1 = oz::INVALID_CAP_ID;
    oz::sys_channel_create(oz::CAP_UNTYPED, &ep_a0, &ep_a1);
    g_abort_ep = ep_a0;

    std::uint32_t abort_baton1 = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(abort_baton1, 50000);
    oz::sys_tcb_bind_baton(abort_tcb, abort_baton1);

    g_abort_worker_ran = false;
    g_abort_result = 0;
    res = oz::sys_baton_switch(abort_baton1);
    bool blocked_in_ipc = (g_abort_worker_ran && res.reason == oz::SchedReason::BLOCKED);

    std::uint32_t rescue_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(rescue_baton, 50000);
    oz::sys_tcb_bind_baton(abort_tcb, rescue_baton);

    res = oz::sys_baton_switch(rescue_baton);
    bool abort_ipc_ok = (blocked_in_ipc && g_abort_result == oz::SYS_ERR_CANCELLED &&
                         (res.reason == oz::SchedReason::EXIT || res.reason == oz::SchedReason::ORPHANED));
    TEST_REPORT("KTEST", "Aborting IPC on Baton bind (SYS_ERR_CANCELLED)", abort_ipc_ok);

    oz::sys_cap_delete(ep_a0);
    oz::sys_cap_delete(ep_a1);

    // 4. Test Dead Client Silent Drop on sys_reply_recv & SchedReason::ORPHANED
    std::uint32_t orphan_srv_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t orphan_srv_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t orphan_srv_stack_top = 0x700030000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, orphan_srv_stack, 0x700030000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(orphan_srv_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_orphan_server_thread), orphan_srv_stack_top);

    std::uint32_t orphan_cli_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t orphan_cli_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t orphan_cli_stack_top = 0x700038000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, orphan_cli_stack, 0x700038000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(orphan_cli_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_orphan_client_thread), orphan_cli_stack_top);

    std::uint32_t ep_s = oz::INVALID_CAP_ID, ep_c = oz::INVALID_CAP_ID;
    oz::sys_channel_create(oz::CAP_UNTYPED, &ep_s, &ep_c);
    g_dead_client_srv_ep = ep_s;
    g_dead_client_cli_ep = ep_c;

    // Start server first with a temporary baton so it enters sys_reply_recv(INVALID_REPLY_TOKEN)
    std::uint32_t srv_init_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(srv_init_baton, 50000);
    oz::sys_tcb_bind_baton(orphan_srv_tcb, srv_init_baton);
    res = oz::sys_baton_switch(srv_init_baton);

    // Now dispatch client with client_baton
    std::uint32_t client_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(client_baton, 100000);
    oz::sys_tcb_bind_baton(orphan_cli_tcb, client_baton);

    g_server_first_recv_ok = false;
    res = oz::sys_baton_switch(client_baton);
    bool dead_client_ok = (g_server_first_recv_ok && res.reason == oz::SchedReason::ORPHANED);
    TEST_REPORT("KTEST", "Dead Client Silent Drop & SchedReason::ORPHANED", dead_client_ok);

    oz::sys_cap_delete(ep_s);
    oz::sys_cap_delete(ep_c);

    // 5. Test Fault IPC: Page Fault Resolution
    std::uint32_t pf_pager_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t pf_pager_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t pf_pager_stack_top = 0x700040000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, pf_pager_stack, 0x700040000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(pf_pager_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_pf_pager_thread), pf_pager_stack_top);

    std::uint32_t pf_client_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t pf_client_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t pf_client_stack_top = 0x700048000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, pf_client_stack, 0x700048000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(pf_client_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_pf_client_thread), pf_client_stack_top);

    std::uint32_t ep_f = oz::INVALID_CAP_ID, ep_p = oz::INVALID_CAP_ID;
    oz::sys_channel_create(oz::CAP_UNTYPED, &ep_f, &ep_p);
    g_pf_pager_ep = ep_p;
    g_pf_frame_cap = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
    oz::sys_tcb_set_fault_endpoint(pf_client_tcb, ep_f);

    // Initial switch to pager so it enters sys_reply_recv(INVALID_REPLY_TOKEN)
    std::uint32_t pager_init_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(pager_init_baton, 50000);
    oz::sys_tcb_bind_baton(pf_pager_tcb, pager_init_baton);
    res = oz::sys_baton_switch(pager_init_baton);

    // Now give client a baton and dispatch client
    std::uint32_t pf_client_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(pf_client_baton, 100000);
    oz::sys_tcb_bind_baton(pf_client_tcb, pf_client_baton);

    g_pf_resolved = false;
    g_pf_fault_addr = 0;
    g_pf_client_success = false;

    res = oz::sys_baton_switch(pf_client_baton);
    bool pf_ok = (g_pf_resolved && g_pf_fault_addr == FAULT_TEST_VADDR && g_pf_client_success && res.reason == oz::SchedReason::YIELD);
    TEST_REPORT("KTEST", "Fault IPC Page Fault Resolution", pf_ok);
    oz::sys_baton_switch(pf_client_baton);

    oz::sys_cap_delete(ep_f);
    oz::sys_cap_delete(ep_p);
    oz::sys_cap_delete(g_pf_frame_cap);

    // 6. Test IRQ Handler & IRQ Baton Separation Architecture
    std::uint32_t dummy_ch_send = 0, dummy_ch_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &dummy_ch_send, &dummy_ch_recv);
    std::uint32_t dummy_irq = oz::sys_irq_handler_create(oz::CAP_UNTYPED, 0x45, dummy_ch_send);

    // Test sys_baton_create_irq without IRQBatonControl -> SYS_ERR_PERM
    std::uint32_t unpriv_irq_baton = oz::sys_baton_create_irq(dummy_irq, oz::CAP_UNTYPED, 50000);
    TEST_REPORT("KTEST", "sys_baton_create_irq privilege enforcement", unpriv_irq_baton == oz::INVALID_CAP_ID);

    // Create valid IRQ batons with CAP_IRQ_CONTROL
    std::uint32_t irq_baton_a = oz::sys_baton_create_irq(oz::CAP_IRQ_CONTROL, oz::CAP_UNTYPED, 50000);
    std::uint32_t irq_baton_b = oz::sys_baton_create_irq(oz::CAP_IRQ_CONTROL, oz::CAP_UNTYPED, 50000);
    TEST_REPORT("KTEST", "sys_baton_create_irq with IRQBatonControl", irq_baton_a != oz::INVALID_CAP_ID && irq_baton_b != oz::INVALID_CAP_ID);

    // Normal baton for type-check and invariant tests
    std::uint32_t normal_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);

    // Capability Type Safety: sys_irq_wait rejecting normal baton
    int type_safety_res = oz::sys_irq_wait(dummy_irq, normal_baton);
    TEST_REPORT("KTEST", "Capability Type Safety (sys_irq_wait rejects normal baton)", type_safety_res == oz::SYS_ERR_INVALID_TYPE);

    // Invariant 1: sys_tcb_bind_baton rejects irq_baton
    std::uint32_t ithread = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    int inv1_res = oz::sys_tcb_bind_baton(ithread, irq_baton_a);
    TEST_REPORT("KTEST", "Invariant 1: sys_tcb_bind_baton rejects irq_baton", inv1_res == oz::SYS_ERR_INVALID);

    // Invariant 2: sys_baton_switch rejects irq_baton
    oz::SchedResult inv2_res = oz::sys_baton_switch(irq_baton_a);
    TEST_REPORT("KTEST", "Invariant 2: sys_baton_switch rejects irq_baton", inv2_res.reason == oz::SYS_BATON_SWITCH_INVALID);

    // sys_baton_set_budget tests
    int set_budget_normal = oz::sys_baton_set_budget(normal_baton, 60000);
    TEST_REPORT("KTEST", "sys_baton_set_budget rejects normal baton", set_budget_normal == oz::SYS_ERR_INVALID_TYPE);
    int set_budget_ok = oz::sys_baton_set_budget(irq_baton_a, 60000);
    TEST_REPORT("KTEST", "sys_baton_set_budget succeeds on irq_baton", set_budget_ok == 0);

    // Configure thread with own_baton
    std::uint32_t istack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t istack_top = 0x700050000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, istack, 0x700050000000ULL, 0x03, 16384);
    g_test_irq_handler_cap = dummy_irq;
    g_test_irq_baton_a = irq_baton_a;
    g_test_irq_baton_b = irq_baton_b;
    oz::sys_tcb_configure(ithread, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_irq_replenish_handler_entry), istack_top);

    std::uint32_t own_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    std::uint32_t inotif = oz::sys_notification_create(oz::CAP_UNTYPED);
    static constexpr std::uint64_t IRQ_BADGE = (1ULL << 12);
    oz::sys_thread_set_notification(ithread, inotif, IRQ_BADGE);
    oz::sys_tcb_bind_baton(ithread, own_baton);

    g_irq_replenish_step = 0;
    oz::sys_baton_inject(own_baton, 50000);
    oz::SchedResult ires = oz::sys_baton_switch(own_baton);
    bool irq_resv_ok = (g_irq_replenish_step == 1);
    TEST_REPORT("KTEST", "Thread sys_irq_wait initial reservation", irq_resv_ok);

    oz::sys_baton_inject(own_baton, 50000);
    oz::sys_tcb_resume(ithread);
    ires = oz::sys_baton_switch(own_baton);
    bool irq_resume_ok = (g_irq_replenish_step == 2);
    TEST_REPORT("KTEST", "Thread sys_irq_wait ping-pong reservation", irq_resume_ok);

    oz::sys_baton_inject(own_baton, 50000);
    oz::sys_tcb_resume(ithread);
    ires = oz::sys_baton_switch(own_baton);
    bool irq_complete_ok = (g_irq_replenish_step == 3);
    TEST_REPORT("KTEST", "Thread handler complete & exit", irq_complete_ok);
}

// =============================================================================
// 3. Dual Baton & Nonblocking IPC Tests
// =============================================================================
static void run_dual_baton_nonblocking_ipc_tests() {
    oz::sys_debug_print("\n\r--- 3. Dual Baton & Nonblocking IPC Tests ---\n\r");

    // 1. ChannelMode combination validation & rejection
    std::uint32_t ep0 = 0, ep1 = 0;
    bool m_bb = oz::sys_channel_create(oz::CAP_UNTYPED, &ep0, &ep1, oz::ChannelMode::BLOCKING, oz::ChannelMode::BLOCKING);
    if (m_bb) { oz::sys_cap_delete(ep0); oz::sys_cap_delete(ep1); }

    bool m_rb = oz::sys_channel_create(oz::CAP_UNTYPED, &ep0, &ep1, oz::ChannelMode::RECV_NONBLOCK, oz::ChannelMode::BLOCKING);
    if (m_rb) { oz::sys_cap_delete(ep0); oz::sys_cap_delete(ep1); }

    bool m_sb = oz::sys_channel_create(oz::CAP_UNTYPED, &ep0, &ep1, oz::ChannelMode::SEND_NONBLOCK, oz::ChannelMode::BLOCKING);
    if (m_sb) { oz::sys_cap_delete(ep0); oz::sys_cap_delete(ep1); }

    bool m_rs = oz::sys_channel_create(oz::CAP_UNTYPED, &ep0, &ep1, oz::ChannelMode::RECV_NONBLOCK, oz::ChannelMode::SEND_NONBLOCK);
    if (m_rs) { oz::sys_cap_delete(ep0); oz::sys_cap_delete(ep1); }

    bool m_rr = oz::sys_channel_create(oz::CAP_UNTYPED, &ep0, &ep1, oz::ChannelMode::RECV_NONBLOCK, oz::ChannelMode::RECV_NONBLOCK);
    bool m_ss = oz::sys_channel_create(oz::CAP_UNTYPED, &ep0, &ep1, oz::ChannelMode::SEND_NONBLOCK, oz::ChannelMode::SEND_NONBLOCK);
    bool m_inv = oz::sys_channel_create(oz::CAP_UNTYPED, &ep0, &ep1, static_cast<oz::ChannelMode>(99), oz::ChannelMode::BLOCKING);

    bool mode_val_ok = (m_bb && m_rb && m_sb && m_rs && !m_rr && !m_ss && !m_inv);
    TEST_REPORT("KTEST", "ChannelMode combination validation & rejection", mode_val_ok);

    // 2. Clean state nonblocking receive (Matrix No. 3)
    std::uint32_t nb_r0 = 0, nb_r1 = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &nb_r0, &nb_r1, oz::ChannelMode::RECV_NONBLOCK, oz::ChannelMode::BLOCKING);
    oz::IPCMessage dummy_recv{};
    int nb_recv_res = oz::sys_reply_and_recv(nb_r0, nullptr, &dummy_recv, 0);
    bool nb_recv_ok = (nb_recv_res == oz::SYS_ERR_WOULDBLOCK);
    TEST_REPORT("KTEST", "Clean state nonblocking receive (SYS_ERR_WOULDBLOCK)", nb_recv_ok);
    oz::sys_cap_delete(nb_r0);
    oz::sys_cap_delete(nb_r1);

    // 3. Nonblocking send without receiver (SEND_NONBLOCK)
    std::uint32_t nb_s0 = 0, nb_s1 = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &nb_s0, &nb_s1, oz::ChannelMode::SEND_NONBLOCK, oz::ChannelMode::BLOCKING);
    oz::IPCMessage nb_smsg{};
    oz::IPCMessage nb_srep{};
    int nb_send_res = oz::sys_call_ext(nb_s0, &nb_smsg, &nb_srep, 0);
    bool nb_send_ok = (nb_send_res == oz::SYS_ERR_WOULDBLOCK);
    TEST_REPORT("KTEST", "Nonblocking send without receiver (SYS_ERR_WOULDBLOCK)", nb_send_ok);
    oz::sys_cap_delete(nb_s0);
    oz::sys_cap_delete(nb_s1);

    // 4. Forced blocking with IPC_FLAG_BLOCK on nonblocking endpoint
    std::uint32_t fb_ep0 = 0, fb_ep1 = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &fb_ep0, &fb_ep1, oz::ChannelMode::RECV_NONBLOCK, oz::ChannelMode::BLOCKING);
    g_forced_block_ep = fb_ep0;

    std::uint32_t fb_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t fb_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t fb_stack_top = 0x700070000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, fb_stack, 0x700070000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(fb_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_forced_block_worker), fb_stack_top);

    std::uint32_t fb_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(fb_baton, 50000);
    oz::sys_tcb_bind_baton(fb_tcb, fb_baton);

    g_forced_block_ran = false;
    oz::SchedResult fb_res = oz::sys_baton_switch(fb_baton);
    bool fb_ok = (g_forced_block_ran && fb_res.reason == oz::SchedReason::BLOCKED);
    TEST_REPORT("KTEST", "Forced blocking with IPC_FLAG_BLOCK on nonblocking endpoint", fb_ok);

    oz::sys_cap_delete(fb_ep0);
    oz::sys_cap_delete(fb_ep1);
    oz::sys_cap_delete(fb_tcb);
    oz::sys_cap_delete(fb_baton);

    // 5. Safety guard: sys_exit rejected while holding borrowed_baton
    std::uint32_t sg_ep0 = 0, sg_ep1 = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &sg_ep0, &sg_ep1);
    g_safety_srv_ep = sg_ep0;

    std::uint32_t sg_srv_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t sg_srv_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t sg_srv_stack_top = 0x700078000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, sg_srv_stack, 0x700078000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(sg_srv_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_safety_guard_worker), sg_srv_stack_top);

    std::uint32_t sg_init_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(sg_init_baton, 50000);
    oz::sys_tcb_bind_baton(sg_srv_tcb, sg_init_baton);
    oz::sys_baton_switch(sg_init_baton);
    oz::sys_tcb_unbind_baton(sg_srv_tcb);
    oz::sys_cap_delete(sg_init_baton);

    g_safety_cli_ep = sg_ep1;
    std::uint32_t sg_cli_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t sg_cli_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t sg_cli_stack_top = 0x700080000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, sg_cli_stack, 0x700080000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(sg_cli_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_safety_guard_client), sg_cli_stack_top);

    std::uint32_t sg_cli_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(sg_cli_baton, 100000);
    oz::sys_tcb_bind_baton(sg_cli_tcb, sg_cli_baton);

    g_safety_guard_exit_res = 0;
    g_safety_client_received_reply = false;
    oz::sys_baton_switch(sg_cli_baton);
    bool sg_ok = (g_safety_guard_exit_res == oz::SYS_ERR_BUSY && g_safety_client_received_reply);
    TEST_REPORT("KTEST", "Safety guard: sys_exit rejected while holding borrowed_baton", sg_ok);

    oz::sys_cap_delete(sg_ep0);
    oz::sys_cap_delete(sg_ep1);
    oz::sys_cap_delete(sg_srv_tcb);
    oz::sys_cap_delete(sg_cli_tcb);
    oz::sys_cap_delete(sg_cli_baton);

    // 6. Dual Baton RPC completion & Notification (Matrix No. 6 / Section 5.1)
    std::uint32_t nb6_s = 0, nb6_c = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &nb6_s, &nb6_c, oz::ChannelMode::RECV_NONBLOCK, oz::ChannelMode::BLOCKING);
    g_nb_srv_ep = nb6_s;

    std::uint32_t nb6_srv_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t nb6_srv_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t nb6_srv_stack_top = 0x700088000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, nb6_srv_stack, 0x700088000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(nb6_srv_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_nb_rpc_worker), nb6_srv_stack_top);

    std::uint32_t nb6_own_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    std::uint32_t nb6_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    static constexpr std::uint64_t BADGE_NB6 = 0xBEEF;
    oz::sys_baton_bind_notification(nb6_own_baton, nb6_notif, BADGE_NB6);
    oz::sys_tcb_bind_baton(nb6_srv_tcb, nb6_own_baton);

    oz::sys_baton_inject(nb6_own_baton, 50000);
    oz::SchedResult nb6_init_res = oz::sys_baton_switch(nb6_own_baton);
    bool srv_blocked = (nb6_init_res.reason == oz::SchedReason::BLOCKED);

    g_nb_cli_ep = nb6_c;
    std::uint32_t nb6_cli_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t nb6_cli_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t nb6_cli_stack_top = 0x700090000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, nb6_cli_stack, 0x700090000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(nb6_cli_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_nb_rpc_client), nb6_cli_stack_top);

    std::uint32_t nb6_cli_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(nb6_cli_baton, 100000);
    oz::sys_tcb_bind_baton(nb6_cli_tcb, nb6_cli_baton);

    g_nb_client_reply_ok = false;
    oz::sys_baton_switch(nb6_cli_baton);

    std::uint64_t notif_val = 0;
    bool notif_ok = (oz::sys_notification_poll(nb6_notif, &notif_val) == 0 && notif_val == BADGE_NB6);

    oz::sys_baton_inject(nb6_own_baton, 50000);
    oz::SchedResult nb6_srv_res = oz::sys_baton_switch(nb6_own_baton);

    bool test6_ok = (srv_blocked && g_nb_client_reply_ok && notif_ok && (nb6_srv_res.reason == oz::SchedReason::EXIT || nb6_srv_res.reason == oz::SchedReason::ORPHANED));
    TEST_REPORT("KTEST", "Dual Baton RPC completion Notification & resume (SYS_ERR_WOULDBLOCK)", test6_ok);

    oz::sys_cap_delete(nb6_s);
    oz::sys_cap_delete(nb6_c);
    oz::sys_cap_delete(nb6_srv_tcb);
    oz::sys_cap_delete(nb6_cli_tcb);
    oz::sys_cap_delete(nb6_own_baton);
    oz::sys_cap_delete(nb6_cli_baton);
    oz::sys_cap_delete(nb6_notif);

    // 7. Dead client with Dual Baton & Notification wakeup (Matrix No. 10-D / Section 5.2)
    std::uint32_t dead_s = 0, dead_c = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &dead_s, &dead_c, oz::ChannelMode::RECV_NONBLOCK, oz::ChannelMode::BLOCKING);
    g_dead_cli_srv_ep = dead_s;

    std::uint32_t dead_srv_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t dead_srv_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t dead_srv_stack_top = 0x700098000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, dead_srv_stack, 0x700098000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(dead_srv_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_dead_client_nb_server), dead_srv_stack_top);

    std::uint32_t dead_own_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    std::uint32_t dead_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    static constexpr std::uint64_t BADGE_DEAD = 0xDEAD;
    oz::sys_baton_bind_notification(dead_own_baton, dead_notif, BADGE_DEAD);
    oz::sys_tcb_bind_baton(dead_srv_tcb, dead_own_baton);

    oz::sys_baton_inject(dead_own_baton, 50000);
    oz::sys_baton_switch(dead_own_baton);

    g_dead_cli_ep = dead_c;
    std::uint32_t dead_cli_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t dead_cli_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t dead_cli_stack_top = 0x7000A0000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, dead_cli_stack, 0x7000A0000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(dead_cli_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_dead_client_call), dead_cli_stack_top);

    std::uint32_t dead_cli_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(dead_cli_baton, 100000);
    oz::sys_tcb_bind_baton(dead_cli_tcb, dead_cli_baton);

    oz::SchedResult d_res1 = oz::sys_baton_switch(dead_cli_baton);
    bool srv_yielded = (d_res1.reason == oz::SchedReason::YIELD);

    oz::sys_cap_delete(dead_cli_tcb);

    oz::SchedResult d_res2 = oz::sys_baton_switch(dead_cli_baton);
    bool orphaned_detected = (d_res2.reason == oz::SchedReason::ORPHANED);

    std::uint64_t dead_notif_val = 0;
    bool dead_notif_ok = (oz::sys_notification_poll(dead_notif, &dead_notif_val) == 0 && dead_notif_val == BADGE_DEAD);

    g_dead_cli_srv_ran = false;
    oz::sys_baton_inject(dead_own_baton, 50000);
    oz::sys_baton_switch(dead_own_baton);

    bool test7_ok = (srv_yielded && orphaned_detected && dead_notif_ok && g_dead_cli_srv_ran);
    TEST_REPORT("KTEST", "Dead client ORPHANED detection & own_baton Notification recovery", test7_ok);

    oz::sys_cap_delete(dead_s);
    oz::sys_cap_delete(dead_c);
    oz::sys_cap_delete(dead_srv_tcb);
    oz::sys_cap_delete(dead_own_baton);
    oz::sys_cap_delete(dead_cli_baton);
    oz::sys_cap_delete(dead_notif);

    // 8. Nested RPC (多段呼び出し / Section 2.4)
    std::uint32_t n_c_s = 0, n_c_c = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &n_c_s, &n_c_c);
    g_nested_srv_c_ep = n_c_s;
    g_nested_call_c_ep = n_c_c;

    std::uint32_t n_b_s = 0, n_b_c = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &n_b_s, &n_b_c);
    g_nested_srv_b_ep = n_b_s;
    g_nested_call_b_ep = n_b_c;

    std::uint32_t srv_c_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t srv_c_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t srv_c_stack_top = 0x7000A8000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, srv_c_stack, 0x7000A8000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(srv_c_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_nested_server_c), srv_c_stack_top);

    std::uint32_t srv_c_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(srv_c_baton, 50000);
    oz::sys_tcb_bind_baton(srv_c_tcb, srv_c_baton);
    oz::sys_baton_switch(srv_c_baton);
    oz::sys_tcb_unbind_baton(srv_c_tcb);
    oz::sys_cap_delete(srv_c_baton);

    std::uint32_t srv_b_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t srv_b_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t srv_b_stack_top = 0x7000B0000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, srv_b_stack, 0x7000B0000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(srv_b_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_nested_server_b), srv_b_stack_top);

    std::uint32_t srv_b_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(srv_b_baton, 50000);
    oz::sys_tcb_bind_baton(srv_b_tcb, srv_b_baton);
    oz::sys_baton_switch(srv_b_baton);
    oz::sys_tcb_unbind_baton(srv_b_tcb);
    oz::sys_cap_delete(srv_b_baton);

    std::uint32_t cli_a_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t cli_a_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t cli_a_stack_top = 0x7000B8000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, cli_a_stack, 0x7000B8000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(cli_a_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_nested_client_a), cli_a_stack_top);

    std::uint32_t cli_a_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(cli_a_baton, 100000);
    oz::sys_tcb_bind_baton(cli_a_tcb, cli_a_baton);

    g_nested_client_res = 0;
    oz::sys_baton_switch(cli_a_baton);
    bool nested_ok = (g_nested_client_res == 210);
    TEST_REPORT("KTEST", "Nested RPC multi-hop baton propagation & distributed unwinding", nested_ok);

    oz::sys_cap_delete(n_c_s);
    oz::sys_cap_delete(n_c_c);
    oz::sys_cap_delete(n_b_s);
    oz::sys_cap_delete(n_b_c);
    oz::sys_cap_delete(srv_c_tcb);
    oz::sys_cap_delete(srv_b_tcb);
    oz::sys_cap_delete(cli_a_tcb);
    oz::sys_cap_delete(cli_a_baton);

    // 9. Call 先行受信 & スケジューラ協調ディスパッチ (Matrix No. 1 / Section 3.1, 4.3)
    std::uint32_t cf_s = 0, cf_c = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &cf_s, &cf_c, oz::ChannelMode::BLOCKING, oz::ChannelMode::BLOCKING);
    g_call_first_srv_ep = cf_s;
    g_call_first_cli_ep = cf_c;

    std::uint32_t cf_cli_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t cf_cli_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t cf_cli_stack_top = 0x7000C0000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, cf_cli_stack, 0x7000C0000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(cf_cli_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_call_first_client), cf_cli_stack_top);

    std::uint32_t cf_cli_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    std::uint32_t cf_cli_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    static constexpr std::uint64_t BADGE_CF_CLI = 0xCA11;
    oz::sys_baton_bind_notification(cf_cli_baton, cf_cli_notif, BADGE_CF_CLI);
    oz::sys_baton_inject(cf_cli_baton, 100000);
    oz::sys_tcb_bind_baton(cf_cli_tcb, cf_cli_baton);

    // Client runs FIRST: blocks on Call 先行 (C-3)
    oz::SchedResult c_res = oz::sys_baton_switch(cf_cli_baton);
    bool cli_blocked = (c_res.reason == oz::SchedReason::BLOCKED);

    std::uint32_t cf_srv_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t cf_srv_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t cf_srv_stack_top = 0x7000C8000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, cf_srv_stack, 0x7000C8000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(cf_srv_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_call_first_server), cf_srv_stack_top);

    std::uint32_t cf_srv_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(cf_srv_baton, 50000);
    oz::sys_tcb_bind_baton(cf_srv_tcb, cf_srv_baton);

    // Server runs on own_baton, receives message (Matrix No. 1),
    // borrows client's baton, fires client baton notification, and yields own_baton BLOCKED!
    oz::SchedResult s_res = oz::sys_baton_switch(cf_srv_baton);
    bool srv_yielded_blocked = (s_res.reason == oz::SchedReason::BLOCKED);

    // Check client baton notification fired
    std::uint64_t cli_notif_val = 0;
    bool cli_notified = (oz::sys_notification_poll(cf_cli_notif, &cli_notif_val) == 0 && cli_notif_val == BADGE_CF_CLI);

    // Sched dispatches client baton (now driving server)
    oz::sys_baton_inject(cf_cli_baton, 100000);
    oz::SchedResult c_res2 = oz::sys_baton_switch(cf_cli_baton);
    // Server completes, direct switches to client, client gets reply and exits!
    bool cli_completed = ((c_res2.reason == oz::SchedReason::EXIT || c_res2.reason == oz::SchedReason::ORPHANED) &&
                          g_call_first_cli_ok && g_call_first_srv_ran);

    bool test9_ok = (cli_blocked && srv_yielded_blocked && cli_notified && cli_completed);
    TEST_REPORT("KTEST", "Call-first receive: own_baton yield & client baton dispatch (Matrix No. 1)", test9_ok);

    oz::sys_cap_delete(cf_s);
    oz::sys_cap_delete(cf_c);
    oz::sys_cap_delete(cf_cli_tcb);
    oz::sys_cap_delete(cf_cli_baton);
    oz::sys_cap_delete(cf_cli_notif);
    oz::sys_cap_delete(cf_srv_tcb);
    oz::sys_cap_delete(cf_srv_baton);
}

// =============================================================================
// 4. Hierarchical Cartesian Scheduling Tests
// =============================================================================
static void run_hierarchical_cartesian_scheduling_tests() {
    oz::sys_debug_print("\n\r--- 4. Hierarchical Cartesian Scheduling Tests ---\n\r");

    // 1. Explicit quantum specification on sys_baton_switch(baton, quantum)
    std::uint32_t ex_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t ex_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t ex_stack_top = 0x7000d0000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, ex_stack, 0x7000d0000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(ex_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_h_explicit_worker), ex_stack_top);
    std::uint32_t ex_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_tcb_bind_baton(ex_tcb, ex_baton);

    g_h_explicit_counter = 0;
    // Note: No sys_baton_inject! Direct switch with explicit quantum 50000 us
    oz::SchedResult res = oz::sys_baton_switch(ex_baton, 50000);
    bool step1_ok = (g_h_explicit_counter == 1 && res.reason == oz::SchedReason::YIELD);

    // Resume to complete
    res = oz::sys_baton_switch(ex_baton);
    bool step2_ok = (g_h_explicit_counter == 2 && (res.reason == oz::SchedReason::EXIT || res.reason == oz::SchedReason::ORPHANED));
    TEST_REPORT("KTEST", "Explicit quantum switch & cooperative yield", step1_ok && step2_ok);

    oz::sys_cap_delete(ex_tcb);
    oz::sys_cap_delete(ex_baton);

    // 2. Deadlock prevention on WAIT_RETURN (SYS_ERR_DEADLK)
    std::uint32_t p_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t p_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t p_stack_top = 0x7000dc000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, p_stack, 0x7000dc000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(p_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_h_deadlock_parent), p_stack_top);
    g_h_parent_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_tcb_bind_baton(p_tcb, g_h_parent_baton);

    g_h_deadlock_res = 0;
    res = oz::sys_baton_switch(g_h_parent_baton, 100000);
    bool deadlk_ok = (g_h_deadlock_res == oz::SYS_ERR_DEADLK);
    TEST_REPORT("KTEST", "Deadlock loop prevention on WAIT_RETURN (SYS_ERR_DEADLK)", deadlk_ok);

    oz::sys_cap_delete(p_tcb);
    oz::sys_cap_delete(g_h_parent_baton);

    // 3. Hierarchical Upper Timeout & Direct Forwarding
    std::uint32_t w_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t w_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t w_stack_top = 0x7000e8000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, w_stack, 0x7000e8000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(w_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_h_worker_thread), w_stack_top);
    g_h_worker_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_tcb_bind_baton(w_tcb, g_h_worker_baton);

    std::uint32_t sub_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t sub_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t sub_stack_top = 0x7000e0000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, sub_stack, 0x7000e0000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(sub_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_h_subsched_thread), sub_stack_top);
    std::uint32_t sub_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_tcb_bind_baton(sub_tcb, sub_baton);

    g_h_worker_started = false;
    g_h_timeout_occurred = false;
    g_h_worker_resumed = false;
    g_h_subsched_finished = false;

    // Give sub-scheduler only 5000 us (5 ms). Worker has 200000 us (200 ms).
    // Sub-scheduler should expire first -> Upper Timeout!
    res = oz::sys_baton_switch(sub_baton, 5000);
    bool upper_timeout_ok = (g_h_worker_started && !g_h_worker_resumed && res.reason == oz::SchedReason::TIMEOUT);
    TEST_REPORT("KTEST", "Hierarchical Upper Timeout preemption (O(1) SplitAt)", upper_timeout_ok);

    // Now resume continuation with 50000 us quantum -> Direct Forwarding to Worker!
    g_h_timeout_occurred = true;
    res = oz::sys_baton_switch(sub_baton, 50000);
    bool direct_forward_ok = (g_h_worker_resumed && g_h_subsched_finished &&
                              (res.reason == oz::SchedReason::EXIT || res.reason == oz::SchedReason::ORPHANED));
    TEST_REPORT("KTEST", "Continuation resumption & Direct Forwarding (ZipMerge)", direct_forward_ok);

    oz::sys_cap_delete(w_tcb);
    oz::sys_cap_delete(g_h_worker_baton);
    oz::sys_cap_delete(sub_tcb);
    oz::sys_cap_delete(sub_baton);

    // 4. Unhandled exception / crash safe unwinding (SchedReason::FAULT)
    std::uint32_t cr_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t cr_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t cr_stack_top = 0x7000f0000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, cr_stack, 0x7000f0000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(cr_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_h_crash_worker), cr_stack_top);
    std::uint32_t cr_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_tcb_bind_baton(cr_tcb, cr_baton);

    res = oz::sys_baton_switch(cr_baton, 50000);
    bool fault_ok = (res.reason == oz::SchedReason::FAULT);
    TEST_REPORT("KTEST", "Unhandled exception returns SchedReason::FAULT", fault_ok);

    oz::sys_cap_delete(cr_tcb);
    oz::sys_cap_delete(cr_baton);
}

// =============================================================================
// 5. Capability Lifecycle New Spec Tests (Phase 6)
// =============================================================================
static volatile bool g_lazy_ipc_cancelled = false;
static std::uint32_t g_lazy_ipc_ep = 0;
static std::uint32_t g_lazy_transferred_cap = 0;

static void test_lazy_ipc_sender_thread() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    req.data[0] = 0x8888;
    req.has_cap = true;
    req.transferred_cap = g_lazy_transferred_cap;
    bool call_ok = oz::sys_call(g_lazy_ipc_ep, &req, &rep);
    if (!call_ok) {
        g_lazy_ipc_cancelled = true;
    }
    for (;;) {
        oz::sys_yield();
    }
}

static volatile bool g_self_destruct_ran = false;
static volatile bool g_self_destruct_cspace_survived = false;
static void test_self_destruct_thread() {
    g_self_destruct_ran = true;
    // 1. Delete own CSpaceCap: thread should survive and continue execution in user mode
    int del_res = oz::sys_cap_delete(oz::CAP_SELF_CSPACE);
    // After CSpaceCap is deleted, further cap syscalls return error because slot is null!
    int res = oz::sys_cap_delete(oz::CAP_SELF_CSPACE);
    if (del_res == 0 && res != 0) {
        g_self_destruct_cspace_survived = true;
    }
    oz::sys_yield();
    oz::sys_baton_exit(0);
}

static volatile bool g_tcb_destruct_ran = false;
static std::uint32_t g_tcb_self_destruct_ut = 0;
static void test_tcb_self_destruct_thread() {
    g_tcb_destruct_ran = true;
    // Revoke the Untyped containing our own TCB!
    oz::sys_untyped_revoke(g_tcb_self_destruct_ut);
    // Should never reach here because TCB was destroyed and abandonCurrentContextTo executed!
    for (;;) {
        oz::sys_yield();
    }
}

static volatile bool g_drop_server_ran = false;
static oz::IPCMessage g_drop_server_recv_msg{};
static std::uint32_t g_drop_server_ep = 0;
static void test_drop_on_fail_server_thread() {
    g_drop_server_ran = true;
    // IPC_RECV_SLOT_NONE で受信待機（受取先スロット未指定 / 拒否）
    oz::sys_reply_and_recv(g_drop_server_ep, nullptr, &g_drop_server_recv_msg, 0, oz::IPC_RECV_SLOT_NONE);
    // クライアントへ返信
    oz::IPCMessage rep{};
    rep.data[0] = 0xABCD;
    oz::sys_reply_and_recv(0, &rep, nullptr);
    for (;;) {
        oz::sys_yield();
    }
}

static void run_capability_lifecycle_new_spec_tests() {
    oz::sys_debug_print("\n\r--- 5. Capability Lifecycle New Spec Tests (Phase 6) ---\n\r");

    // 1. Hierarchical CDT & Reparenting
    {
        std::uint32_t ut_child = oz::sys_untyped_retype(
            oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory), 64 * 1024);
        if (ut_child != oz::INVALID_CAP_ID) {
            std::uint32_t notif_child = oz::sys_notification_create(ut_child);
            if (notif_child != oz::INVALID_CAP_ID) {
                std::uint32_t notif_copy = oz::sys_cap_mint(notif_child, static_cast<std::uint32_t>(oz::CapRights::ALL));
                if (notif_copy != oz::INVALID_CAP_ID) {
                    // Delete intermediate node (notif_child)
                    // notif_copy should be reparented to ut_child and remain functional
                    int del_res = oz::sys_cap_delete(notif_child);
                    oz::sys_notification_signal(notif_copy, 0x123);
                    std::uint64_t polled = 0;
                    int poll_res = oz::sys_notification_poll(notif_copy, &polled);
                    bool reparent_ok = (del_res == 0 && poll_res == 0 && polled == 0x123);

                    // Revoke root of subtree (ut_child)
                    // notif_copy should be pruned
                    oz::sys_untyped_revoke(ut_child);
                    bool pruned_ok = (oz::sys_notification_signal(notif_copy, 0x456) != 0);

                    TEST_REPORT("KTEST", "Hierarchical CDT reparenting & subtree revoke", reparent_ok && pruned_ok);
                } else {
                    TEST_REPORT("KTEST", "Hierarchical CDT reparenting & subtree revoke", false);
                    oz::sys_cap_delete(notif_child);
                }
            } else {
                TEST_REPORT("KTEST", "Hierarchical CDT reparenting & subtree revoke", false);
            }
            oz::sys_cap_delete(ut_child);
        } else {
            TEST_REPORT("KTEST", "Hierarchical CDT reparenting & subtree revoke", false);
        }
    }

    // 2. Untyped Linear Allocation & Auto-reset
    {
        std::uint32_t u_test = oz::sys_untyped_retype(
            oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory), 64 * 1024);
        if (u_test != oz::INVALID_CAP_ID) {
            // Linear prevention check
            std::uint32_t dup = oz::sys_cap_mint(u_test, static_cast<std::uint32_t>(oz::CapRights::ALL));
            bool linear_ok = (dup == oz::INVALID_CAP_ID);

            // Allocate 16 pages of frames (64KB total)
            std::uint32_t frames[16];
            bool alloc_ok = true;
            for (int i = 0; i < 16; ++i) {
                frames[i] = oz::sys_frame_alloc(u_test, 1);
                if (frames[i] == oz::INVALID_CAP_ID) {
                    alloc_ok = false;
                }
            }
            // 17th allocation must fail
            std::uint32_t overflow_frame = oz::sys_frame_alloc(u_test, 1);
            bool overflow_ok = (overflow_frame == oz::INVALID_CAP_ID);

            // Delete all 16 frames -> auto-reset watermark to 0
            for (int i = 0; i < 16; ++i) {
                if (frames[i] != oz::INVALID_CAP_ID) {
                    oz::sys_cap_delete(frames[i]);
                }
            }

            // Verify capacity recovered: allocating a new frame succeeds!
            std::uint32_t re_frame = oz::sys_frame_alloc(u_test, 1);
            bool recovered_ok = (re_frame != oz::INVALID_CAP_ID);
            if (re_frame != oz::INVALID_CAP_ID) {
                oz::sys_cap_delete(re_frame);
            }

            TEST_REPORT("KTEST", "Untyped linear allocation & auto-reset watermark",
                        linear_ok && alloc_ok && overflow_ok && recovered_ok);
            oz::sys_cap_delete(u_test);
        } else {
            TEST_REPORT("KTEST", "Untyped linear allocation & auto-reset watermark", false);
        }
    }

    // 3. Frame Mapping Lifecycle & Automatic Unmap on Delete
    {
        std::uint32_t f = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
        if (f != oz::INVALID_CAP_ID) {
            constexpr std::uint64_t TEST_VA = 0x600050000000ULL;
            int map_res = oz::sys_vspace_map_with_pagetables(
                oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, f, TEST_VA, 0x01, 4096);
            if (map_res == 0) {
                *reinterpret_cast<volatile std::uint64_t*>(TEST_VA) = 0xDEADBEEFCAFEBABEULL;
                bool read_ok = (*reinterpret_cast<volatile std::uint64_t*>(TEST_VA) == 0xDEADBEEFCAFEBABEULL);

                // Delete frame capability -> kernel automatically unmaps
                int del_res = oz::sys_cap_delete(f);

                // Unmapping the virtual address now must fail (already unmapped)
                int unmap_again = oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, TEST_VA);
                bool auto_unmapped = (del_res == 0 && unmap_again != 0);

                TEST_REPORT("KTEST", "Frame mapping lifecycle & auto-unmap on delete", read_ok && auto_unmapped);
            } else {
                TEST_REPORT("KTEST", "Frame mapping lifecycle & auto-unmap on delete", false);
                oz::sys_cap_delete(f);
            }
        } else {
            TEST_REPORT("KTEST", "Frame mapping lifecycle & auto-unmap on delete", false);
        }
    }

    // 4. Inline Caps & CSpace Teardown
    {
        std::uint32_t cs = oz::sys_cspace_create(oz::CAP_UNTYPED);
        if (cs != oz::INVALID_CAP_ID) {
            // Create child IOPort inside child CSpace
            std::uint32_t io_child = oz::sys_ioport_create(oz::CAP_ROOT_IOPORT, oz::CAP_UNTYPED, 0x3F8, 8);
            bool io_created = (io_child != oz::INVALID_CAP_ID);
            if (io_created) {
                oz::sys_cspace_cap_move(cs, 10, io_child);
            }

            // Create IRQHandler for vector 0x48 inside child CSpace
            std::uint32_t irq_h = oz::sys_irq_handler_create(oz::CAP_UNTYPED, 0x48, 0);
            bool irq_created = (irq_h != oz::INVALID_CAP_ID);
            if (irq_created) {
                oz::sys_cspace_cap_move(cs, 11, irq_h);
            }

            // Tear down entire child CSpace: must reverse-unbind and free without panic or leak
            int cs_del = oz::sys_cap_delete(cs);

            // Verify vector 0x48 is reset and can be registered again without collision
            std::uint32_t irq_h2 = oz::sys_irq_handler_create(oz::CAP_UNTYPED, 0x48, 0);
            bool irq_recreated = (irq_h2 != oz::INVALID_CAP_ID);
            if (irq_recreated) {
                oz::sys_cap_delete(irq_h2);
            }

            TEST_REPORT("KTEST", "Inline caps & CSpace teardown reverse cleanup",
                        io_created && irq_created && cs_del == 0 && irq_recreated);
        } else {
            TEST_REPORT("KTEST", "Inline caps & CSpace teardown reverse cleanup", false);
        }
    }

    // 5. IPC Lazy Move on Cancel
    {
        std::uint32_t lep0 = 0, lep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &lep0, &lep1)) {
            g_lazy_ipc_ep = lep0;
            g_lazy_transferred_cap = oz::sys_notification_create(oz::CAP_UNTYPED);

            std::uint32_t sender_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
            std::uint32_t sender_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
            std::uint64_t sender_stack_top = 0x700070000000ULL + 16384;
            oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, sender_stack, 0x700070000000ULL, 0x03, 16384);
            oz::sys_tcb_configure(sender_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                                  reinterpret_cast<std::uint64_t>(test_lazy_ipc_sender_thread), sender_stack_top);
            std::uint32_t sender_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
            oz::sys_baton_inject(sender_baton, 50000);
            oz::sys_tcb_bind_baton(sender_tcb, sender_baton);

            // Switch to sender: sender calls sys_call with transferred cap and blocks because lep1 has no receiver
            oz::sys_baton_switch(sender_baton);

            // Cancel sender's IPC while blocked
            oz::sys_tcb_cancel_ipc(sender_tcb);

            // Switch back to sender so sys_call unblocks with error
            oz::sys_baton_switch(sender_baton);

            // Verify: transferred cap is still in CSpace and functional!
            oz::sys_notification_signal(g_lazy_transferred_cap, 0x777);
            std::uint64_t polled_lazy = 0;
            bool cap_intact = (oz::sys_notification_poll(g_lazy_transferred_cap, &polled_lazy) == 0 && polled_lazy == 0x777);

            TEST_REPORT("KTEST", "IPC lazy capability transfer preserved on cancel",
                        g_lazy_ipc_cancelled && cap_intact);

            oz::sys_cap_delete(g_lazy_transferred_cap);
            oz::sys_cap_delete(sender_tcb);
            oz::sys_cap_delete(sender_baton);
            oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, sender_stack);
            oz::sys_cap_delete(sender_stack);
            oz::sys_cap_delete(lep0);
            oz::sys_cap_delete(lep1);
        } else {
            TEST_REPORT("KTEST", "IPC lazy capability transfer preserved on cancel", false);
        }
    }

    // 6. Self-destruction Graceful Transition
    {
        g_self_destruct_ran = false;
        g_self_destruct_cspace_survived = false;
        std::uint32_t sd_cs = oz::sys_cspace_create(oz::CAP_UNTYPED);
        std::uint32_t sd_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
        std::uint32_t sd_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
        std::uint64_t sd_stack_top = 0x700080000000ULL + 16384;
        oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, sd_stack, 0x700080000000ULL, 0x03, 16384);

        // Put sd_cs into its own CSpace at CAP_SELF_CSPACE (245)
        oz::sys_cspace_cap_insert(sd_cs, oz::CAP_SELF_CSPACE, sd_cs, static_cast<std::uint32_t>(oz::CapRights::ALL));

        oz::sys_tcb_configure(sd_tcb, sd_cs, oz::CAP_SELF_VSPACE,
                              reinterpret_cast<std::uint64_t>(test_self_destruct_thread), sd_stack_top);
        std::uint32_t sd_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
        oz::sys_baton_inject(sd_baton, 50000);
        oz::sys_tcb_bind_baton(sd_tcb, sd_baton);

        // Switch to self-destruct thread
        auto switch_res = oz::sys_baton_switch(sd_baton);

        bool cspace_destruct_ok = g_self_destruct_ran && g_self_destruct_cspace_survived;

        oz::sys_cap_delete(sd_tcb);
        oz::sys_cap_delete(sd_baton);
        oz::sys_cap_delete(sd_cs);
        oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, 0x700080000000ULL);
        oz::sys_cap_delete(sd_stack);

        // Now test TCB self-destruction via abandonCurrentContextTo
        g_tcb_destruct_ran = false;
        std::uint32_t ut_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory), 64 * 1024);
        std::uint32_t tcb2 = oz::sys_untyped_retype(ut_tcb, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
        std::uint32_t b2 = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
        oz::sys_baton_inject(b2, 50000);
        std::uint32_t stack2 = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
        std::uint64_t stack2_top = 0x700088000000ULL + 16384;
        oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, stack2, 0x700088000000ULL, 0x03, 16384);

        g_tcb_self_destruct_ut = ut_tcb;
        oz::sys_tcb_configure(tcb2, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                              reinterpret_cast<std::uint64_t>(test_tcb_self_destruct_thread), stack2_top);
        oz::sys_tcb_bind_baton(tcb2, b2);

        auto switch2_res = oz::sys_baton_switch(b2);
        bool tcb_destruct_ok = g_tcb_destruct_ran && (switch2_res.reason != oz::SchedReason::NONE);

        // Untyped should have auto-reset watermark because tcb2 was pruned
        std::uint32_t re_alloc_frame = oz::sys_frame_alloc(ut_tcb, 1);
        bool ut_reset_ok = (re_alloc_frame != oz::INVALID_CAP_ID);
        if (re_alloc_frame != oz::INVALID_CAP_ID) {
            oz::sys_cap_delete(re_alloc_frame);
        }

        oz::sys_cap_delete(b2);
        oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, 0x700088000000ULL);
        oz::sys_cap_delete(stack2);
        oz::sys_cap_delete(ut_tcb);

        TEST_REPORT("KTEST", "Self-destruction graceful transition & context abandonment",
                    cspace_destruct_ok && tcb_destruct_ok && ut_reset_ok);
    }

    // 7. IPC Capability Drop-on-Fail (Invalid/Zero Slot drops cap but delivers message)
    {
        g_drop_server_ran = false;
        g_drop_server_recv_msg = {};
        std::uint32_t dep0 = 0, dep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &dep0, &dep1)) {
            g_drop_server_ep = dep1;
            std::uint32_t notif_cap = oz::sys_notification_create(oz::CAP_UNTYPED);

            std::uint32_t s_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
            std::uint32_t s_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
            constexpr std::uint64_t S_STACK_VA = 0x700090000000ULL;
            oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, s_stack, S_STACK_VA, 0x03, 16384);
            oz::sys_tcb_configure(s_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                                  reinterpret_cast<std::uint64_t>(test_drop_on_fail_server_thread), S_STACK_VA + 16384);
            std::uint32_t s_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
            oz::sys_baton_inject(s_baton, 50000);
            oz::sys_tcb_bind_baton(s_tcb, s_baton);

            // Switch to server: server enters sys_reply_and_recv with recv_slot = 0
            oz::sys_baton_switch(s_baton);

            // Client sends message with has_cap = true
            oz::IPCMessage send_msg{};
            send_msg.data[0] = 0x1234;
            send_msg.has_cap = true;
            send_msg.transferred_cap = notif_cap;
            oz::IPCMessage recv_rep{};

            bool call_res = oz::sys_call(dep0, &send_msg, &recv_rep);

            // Verify:
            // 1. sys_call succeeded (return true) and received reply
            bool call_ok = (call_res && recv_rep.data[0] == 0xABCD);
            // 2. Server received message data, but cap was dropped
            bool server_ok = g_drop_server_ran &&
                             (g_drop_server_recv_msg.data[0] == 0x1234) &&
                             (!g_drop_server_recv_msg.has_cap) &&
                             (g_drop_server_recv_msg.transferred_cap == 0);
            // 3. Sender cap was destroyed in CSpace (delete fails)
            int del_after_drop = oz::sys_cap_delete(notif_cap);
            bool cap_destroyed = (del_after_drop != 0);

            TEST_REPORT("KTEST", "IPC drop capability on slot failure but deliver message",
                        call_ok && server_ok && cap_destroyed);

            oz::sys_cap_delete(s_tcb);
            oz::sys_cap_delete(s_baton);
            oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, S_STACK_VA);
            oz::sys_cap_delete(s_stack);
            oz::sys_cap_delete(dep0);
            oz::sys_cap_delete(dep1);
        } else {
            TEST_REPORT("KTEST", "IPC drop capability on slot failure but deliver message", false);
        }
    }

    // 8. IPC transfer slot 0 protection (transferred_cap=0 does not destroy CAP_SELF_VSPACE)
    {
        std::uint32_t ep0 = 0, ep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &ep0, &ep1)) {
            oz::IPCMessage send_msg{};
            send_msg.has_cap = true;
            send_msg.transferred_cap = 0; // slot 0
            oz::IPCMessage recv_rep{};
            bool call_res = oz::sys_call(ep0, &send_msg, &recv_rep);
            oz::CapInfo info{};
            bool vspace_intact = (oz::sys_cap_identify(oz::CAP_SELF_VSPACE, &info) == 0 && info.type != 0);
            TEST_REPORT("KTEST", "IPC transfer slot 0 protection (CAP_SELF_VSPACE preserved)", !call_res && vspace_intact);
            oz::sys_cap_delete(ep0);
            oz::sys_cap_delete(ep1);
        } else {
            TEST_REPORT("KTEST", "IPC transfer slot 0 protection (CAP_SELF_VSPACE preserved)", false);
        }
    }

    // 9. IPC transfer non-TRANSFER cap rejected without deleting source cap
    {
        std::uint32_t ep0 = 0, ep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &ep0, &ep1)) {
            std::uint32_t notif_cap = oz::sys_notification_create(oz::CAP_UNTYPED);
            // Mint with READ | REVOKE (no TRANSFER)
            std::uint32_t no_xfer = oz::sys_cap_mint(notif_cap, static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::REVOKE));
            if (no_xfer != oz::INVALID_CAP_ID) {
                oz::IPCMessage send_msg{};
                send_msg.has_cap = true;
                send_msg.transferred_cap = no_xfer;
                oz::IPCMessage recv_rep{};
                bool call_res = oz::sys_call(ep0, &send_msg, &recv_rep);
                // Cap must still exist
                bool cap_survived = (oz::sys_cap_delete(no_xfer) == 0);
                TEST_REPORT("KTEST", "IPC non-TRANSFER cap transfer rejected and source preserved", !call_res && cap_survived);
            } else {
                TEST_REPORT("KTEST", "IPC non-TRANSFER cap transfer rejected and source preserved", false);
            }
            oz::sys_cap_delete(notif_cap);
            oz::sys_cap_delete(ep0);
            oz::sys_cap_delete(ep1);
        } else {
            TEST_REPORT("KTEST", "IPC non-TRANSFER cap transfer rejected and source preserved", false);
        }
    }

    // 10. IPC transfer reserved slot protection
    {
        std::uint32_t ep0 = 0, ep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &ep0, &ep1)) {
            oz::IPCMessage send_msg{};
            send_msg.has_cap = true;
            send_msg.transferred_cap = oz::CAP_UNTYPED; // 240
            oz::IPCMessage recv_rep{};
            bool call_res = oz::sys_call(ep0, &send_msg, &recv_rep);
            oz::CapInfo info{};
            bool untyped_survived = (oz::sys_cap_identify(oz::CAP_UNTYPED, &info) == 0 && info.type != 0);
            TEST_REPORT("KTEST", "IPC reserved slot transfer rejected and CAP_UNTYPED preserved", !call_res && untyped_survived);
            oz::sys_cap_delete(ep0);
            oz::sys_cap_delete(ep1);
        } else {
            TEST_REPORT("KTEST", "IPC reserved slot transfer rejected and CAP_UNTYPED preserved", false);
        }
    }

    // 11. Abolished syscall 5 returns SYS_ERR_INVALID
    {
        std::int64_t sys5_res = static_cast<std::int64_t>(oz::syscall(5, 0, 0, 0, 0, 0, 0));
        TEST_REPORT("KTEST", "Abolished syscall 5 returns SYS_ERR_INVALID", sys5_res == oz::SYS_ERR_INVALID);
    }
}

// =============================================================================
// 6. Hierarchical PageTable & VSpace Integration Tests
// =============================================================================
static void run_hierarchical_pagetable_tests() {
    oz::sys_debug_print("\n\r--- 6. Hierarchical PageTable & VSpace Integration Tests ---\n\r");

    // 1. test_separated_untyped_zero_padding
    {
        // 64KB (16 frames) Untyped A and 4KB Untyped B
        std::uint32_t ut_A = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory), 64 * 1024);
        std::uint32_t ut_B = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory), 4096);

        bool setup_ok = (ut_A != oz::INVALID_CAP_ID && ut_B != oz::INVALID_CAP_ID);

        // Allocate 16 frames from ut_A with zero padding waste
        std::uint32_t frames[16] = {};
        bool all_frames_ok = true;
        for (int i = 0; i < 16; ++i) {
            frames[i] = oz::sys_frame_alloc(ut_A, 1);
            if (frames[i] == oz::INVALID_CAP_ID) {
                all_frames_ok = false;
                break;
            }
            // In between, create a 56B PageTableObject from ut_B (backed by a frame from CAP_UNTYPED)
            if (i == 4) {
                std::uint32_t bk = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
                std::uint32_t pt = oz::sys_pagetable_create(ut_B, bk, 1);
                if (pt == oz::INVALID_CAP_ID) all_frames_ok = false;
                oz::sys_cap_delete(pt);
                oz::sys_cap_delete(bk);
            }
        }

        // ut_A should now be completely full (allocating one more must fail)
        std::uint32_t extra = oz::sys_frame_alloc(ut_A, 1);
        bool full_ok = (extra == oz::INVALID_CAP_ID);

        for (int i = 0; i < 16; ++i) {
            if (frames[i] != oz::INVALID_CAP_ID) oz::sys_cap_delete(frames[i]);
        }
        oz::sys_cap_delete(ut_A);
        oz::sys_cap_delete(ut_B);

        TEST_REPORT("KTEST", "Zero padding waste with separated physical/kernel untypeds", setup_ok && all_frames_ok && full_ok);
    }

    // 2. test_pt_backing_frame_exclusivity
    {
        std::uint32_t f_orig = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
        std::uint32_t f_copy = oz::sys_cap_mint(f_orig, static_cast<std::uint32_t>(oz::CapRights::ALL));

        // Both f_orig (has child) and f_copy (not original) must be rejected
        std::uint32_t pt_bad1 = oz::sys_pagetable_create(oz::CAP_UNTYPED, f_orig, 1);
        std::uint32_t pt_bad2 = oz::sys_pagetable_create(oz::CAP_UNTYPED, f_copy, 1);

        bool copy_rejected = (pt_bad1 == oz::INVALID_CAP_ID && pt_bad2 == oz::INVALID_CAP_ID);

        // Delete f_orig to reparent f_copy to Untyped root
        oz::sys_cap_delete(f_orig);

        // Reparented f_copy still lacks SLOT_FLAG_ORIGINAL -> must be rejected
        std::uint32_t pt_bad3 = oz::sys_pagetable_create(oz::CAP_UNTYPED, f_copy, 1);
        bool reparent_rejected = (pt_bad3 == oz::INVALID_CAP_ID);
        oz::sys_cap_delete(f_copy);

        // Valid original frame
        std::uint32_t f_valid = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
        std::uint32_t pt_valid = oz::sys_pagetable_create(oz::CAP_UNTYPED, f_valid, 1);
        bool valid_created = (pt_valid != oz::INVALID_CAP_ID);

        // While bound, f_valid cannot be mapped or duplicated
        int map_bound = oz::sys_pagetable_map_frame(pt_valid, 0, f_valid, 0x01);
        std::uint32_t mint_bound = oz::sys_cap_mint(f_valid, static_cast<std::uint32_t>(oz::CapRights::ALL));
        bool bound_exclusive = (map_bound != 0 && mint_bound == oz::INVALID_CAP_ID);

        oz::sys_cap_delete(pt_valid);
        oz::sys_cap_delete(f_valid);

        TEST_REPORT("KTEST", "Backing frame exclusivity & reparenting elevation prevention",
                    copy_rejected && reparent_rejected && valid_created && bound_exclusive);
    }

    // 3. test_shared_pagetable_mapping
    {
        std::uint32_t bk_shared = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
        std::uint32_t pt_shared = oz::sys_pagetable_create(oz::CAP_UNTYPED, bk_shared, 1);

        std::uint32_t data_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
        oz::sys_pagetable_map_frame(pt_shared, 0, data_frame, 0x03);

        // Mint read-only copy (drop WRITE right)
        std::uint32_t pt_ro = oz::sys_cap_mint(pt_shared,
            static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::GRANT));

        constexpr std::uint64_t VA_RW = 0x380000000000ULL;
        constexpr std::uint64_t VA_RO = 0x380000200000ULL; // 2MB offset (same Level 2 PD)

        int map_rw = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, pt_shared, VA_RW, 0x01, 4096);
        int map_ro = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, pt_ro, VA_RO, 0x00, 4096);

        bool map_both_ok = (map_rw == 0 && map_ro == 0);

        // Write via RW alias, read via RO alias
        *reinterpret_cast<volatile std::uint32_t*>(VA_RW) = 0xBEEFCAFE;
        std::uint32_t read_back = *reinterpret_cast<volatile std::uint32_t*>(VA_RO);
        bool shared_data_ok = (read_back == 0xBEEFCAFE);

        // Attempting to map frame through pt_ro must fail due to missing WRITE right
        std::uint32_t extra_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
        int ro_map_attempt = oz::sys_pagetable_map_frame(pt_ro, 1, extra_frame, 0x01);
        bool ro_enforced = (ro_map_attempt != 0);

        oz::sys_cap_unmap(pt_shared);
        oz::sys_cap_unmap(pt_ro);

        oz::sys_cap_delete(pt_shared);
        oz::sys_cap_delete(pt_ro);
        oz::sys_cap_delete(data_frame);
        oz::sys_cap_delete(extra_frame);
        oz::sys_cap_delete(bk_shared);

        TEST_REPORT("KTEST", "Shared PageTable mapping & cross-hierarchy privilege protection",
                    map_both_ok && shared_data_ok && ro_enforced);
    }

    // 4. test_hierarchical_pt_lifecycle_and_unlimited_vspace
    {
        // 4.1 Single-slot unmap and remap
        std::uint32_t f = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
        constexpr std::uint64_t VA1 = 0x390000000000ULL;
        constexpr std::uint64_t VA2 = 0x390000001000ULL;

        int m1 = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, f, VA1, 0x01, 4096);
        int u1 = oz::sys_cap_unmap(f);
        int m2 = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, f, VA2, 0x01, 4096);
        int u2 = oz::sys_cap_unmap(f);
        oz::sys_cap_delete(f);
        bool remap_ok = (m1 == 0 && u1 == 0 && m2 == 0 && u2 == 0);

        // 4.2 Pattern 1: Delete PageTable releases backing frame
        std::uint32_t bk1 = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
        std::uint32_t pt1 = oz::sys_pagetable_create(oz::CAP_UNTYPED, bk1, 1);
        oz::sys_cap_delete(pt1); // Disconnects backing frame
        // Now bk1 can be mapped as normal data frame
        int m_bk1 = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, bk1, 0x390000002000ULL, 0x01, 4096);
        *reinterpret_cast<volatile std::uint32_t*>(0x390000002000ULL) = 0x12345678;
        bool p1_ok = (m_bk1 == 0 && *reinterpret_cast<volatile std::uint32_t*>(0x390000002000ULL) == 0x12345678);
        oz::sys_cap_unmap(bk1);
        oz::sys_cap_delete(bk1);

        // 4.3 Pattern 2: Delete backing frame unbinds and unmaps PT
        std::uint32_t bk2 = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
        std::uint32_t pt2 = oz::sys_pagetable_create(oz::CAP_UNTYPED, bk2, 1);
        int m_pt2 = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, pt2, 0x390000200000ULL, 0x01, 4096);
        oz::sys_cap_delete(bk2); // Backing deleted -> PT unbound and unmapped from parent
        // Subsequent map of pt2 must fail
        int m_pt2_again = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, pt2, 0x390000400000ULL, 0x01, 4096);
        bool p2_ok = (m_pt2 == 0 && m_pt2_again != 0);
        oz::sys_cap_delete(pt2);

        // 4.4 Unlimited VSpace allocation (> 256 instances)
        bool unlimited_ok = true;
        for (int i = 0; i < 270; ++i) {
            std::uint32_t vs = oz::sys_vspace_create(oz::CAP_UNTYPED);
            if (vs == oz::INVALID_CAP_ID) {
                unlimited_ok = false;
                break;
            }
            oz::sys_cap_delete(vs);
        }

        TEST_REPORT("KTEST", "Hierarchical PageTable lifecycle & unlimited VSpace table",
                    remap_ok && p1_ok && p2_ok && unlimited_ok);
    }

    // 5. test_integer_truncation_rejection_and_none_rights
    {
        // 5.1 Integer truncation test on sys_pagetable_map_frame (Syscall 4)
        std::uint32_t bk = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
        std::uint32_t pt = oz::sys_pagetable_create(oz::CAP_UNTYPED, bk, 1);
        std::uint32_t f = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);

        // index = 65536 (0x10000), 512: must be rejected with -1
        int res_trunc1 = oz::sys_pagetable_map_frame(pt, 65536, f, 0x01);
        int res_trunc2 = oz::sys_pagetable_map_frame(pt, 512, f, 0x01);
        bool trunc_map_frame_rejected = (res_trunc1 == -1 && res_trunc2 == -1);

        // Valid map at 0 works
        int res_valid = oz::sys_pagetable_map_frame(pt, 0, f, 0x01);
        bool valid_map_ok = (res_valid == 0);

        // 5.2 Integer truncation test on sys_pagetable_map (Syscall 25)
        std::uint32_t bk_pd = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
        std::uint32_t pd = oz::sys_pagetable_create(oz::CAP_UNTYPED, bk_pd, 2);
        int res_trunc_pd = oz::sys_pagetable_map(pd, 65536, pt);
        int res_trunc_pd512 = oz::sys_pagetable_map(pd, 512, pt);
        bool trunc_map_pd_rejected = (res_trunc_pd == -1 && res_trunc_pd512 == -1);

        // 5.3 CapRights::NONE mint rights check
        std::uint32_t f_none = oz::sys_cap_mint(f, static_cast<std::uint32_t>(oz::CapRights::NONE));
        oz::CapInfo info_none{};
        int id_res = oz::sys_cap_identify(f_none, &info_none);
        bool none_rights_ok = (id_res == 0 && info_none.rights == 0);

        // Attempting to map f_none (which lacks READ right) must fail
        int map_none_attempt = oz::sys_pagetable_map_frame(pt, 1, f_none, 0x01);
        bool none_rejected = (map_none_attempt != 0);

        oz::sys_cap_unmap(f);
        oz::sys_cap_delete(f_none);
        oz::sys_cap_delete(f);
        oz::sys_cap_delete(pt);
        oz::sys_cap_delete(bk);
        oz::sys_cap_delete(pd);
        oz::sys_cap_delete(bk_pd);

        TEST_REPORT("KTEST", "Truncation guard on 64-bit index & CapRights::NONE enforcement",
                    trunc_map_frame_rejected && valid_map_ok && trunc_map_pd_rejected && none_rights_ok && none_rejected);
    }
}

} // namespace oz::ktest

extern "C" void _start() {
    oz::g_slot_allocator.reset();
    oz::sys_debug_print("\n\r========================================================\n\r");
    oz::sys_debug_print(" [OZ_OS] Standalone Kernel Test Runner (ktest)\n\r");
    oz::sys_debug_print("========================================================\n\r");

    oz::ktest::run_capability_semantics_tests();
    oz::ktest::run_baton_scheduling_tests();
    oz::ktest::run_dual_baton_nonblocking_ipc_tests();
    oz::ktest::run_hierarchical_cartesian_scheduling_tests();
    oz::ktest::run_capability_lifecycle_new_spec_tests();
    oz::ktest::run_hierarchical_pagetable_tests();

    oz::test::print_summary("KTEST");
    oz::test::exit_qemu();
}
