#include "syscall.hpp"
#include "../common/test_harness.hpp"

namespace oz::ktest {

static volatile bool g_once_server_received = false;
static std::uint32_t g_once_server_ep = 0;
static void test_once_server_thread() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    rep.data[0] = 0x5555;
    oz::ReplyToken tok = oz::sys_reply_recv(oz::INVALID_REPLY_TOKEN, g_once_server_ep, nullptr, &req);
    g_once_server_received = (tok != oz::INVALID_REPLY_TOKEN);
    oz::sys_reply_recv(tok, g_once_server_ep, &rep, &req);
    oz::sys_exit(0);
}

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

    // Test Once Capability Auto-consumption
    std::uint32_t o_ep0 = 0, o_ep1 = 0;
    if (oz::sys_channel_create(oz::CAP_UNTYPED, &o_ep0, &o_ep1)) {
        g_once_server_ep = o_ep1;

        std::uint32_t s_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
        std::uint32_t s_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
        std::uint64_t s_stack_top = 0x700060000000ULL + 16384;
        oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, s_stack, 0x700060000000ULL, 0x03, 16384);
        oz::sys_tcb_configure(s_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                              reinterpret_cast<std::uint64_t>(test_once_server_thread), s_stack_top);

        std::uint32_t s_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
        oz::sys_baton_inject(s_baton, 50000);
        oz::sys_tcb_bind_baton(s_tcb, s_baton);
        oz::sys_baton_switch(s_baton);

        std::uint32_t once_cap = oz::sys_cap_mint(o_ep0, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::ONCE));
        if (once_cap != oz::INVALID_CAP_ID) {
            oz::IPCMessage req{};
            oz::IPCMessage rep{};
            req.data[0] = 0x1234;
            bool call_ok = oz::sys_call(once_cap, &req, &rep);
            bool slot_deleted = (oz::sys_cap_delete(once_cap) != 0);
            TEST_REPORT("KTEST", "Once Capability auto-consumption on sys_call", call_ok && slot_deleted && rep.data[0] == 0x5555);
        } else {
            TEST_REPORT("KTEST", "Once Capability auto-consumption on sys_call", false);
        }
        oz::sys_cap_delete(o_ep0);
        oz::sys_cap_delete(o_ep1);
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
        oz::syscall(2, g_abort_ep, reinterpret_cast<std::uint64_t>(&msg), 0, 0, 0, 0));
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
static constexpr std::uint64_t FAULT_TEST_VADDR = 0x65000000ULL;

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

static void test_irq_replenish_handler_entry() {
    g_irq_replenish_step = 1;
    oz::sys_irq_wait(g_test_irq_handler_cap);
    g_irq_replenish_step = 2;
    oz::sys_irq_wait(g_test_irq_handler_cap);
    g_irq_replenish_step = 3;
    oz::sys_exit(0);
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

    // 6. Test InterruptThread & IRQ replenishment
    std::uint32_t dummy_ch_send = 0, dummy_ch_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &dummy_ch_send, &dummy_ch_recv);
    std::uint32_t dummy_irq = oz::sys_irq_handler_create(oz::CAP_UNTYPED, 0x45, dummy_ch_send);

    int type_safety_res = oz::sys_irq_wait(dummy_irq);
    bool type_safety_ok = (type_safety_res == oz::SYS_ERR_INVALID_TYPE);
    TEST_REPORT("KTEST", "Capability Type Safety (sys_irq_wait rejected)", type_safety_ok);

    std::uint32_t ithread = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::InterruptThread), 0);
    std::uint32_t istack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t istack_top = 0x700050000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, istack, 0x700050000000ULL, 0x03, 16384);
    g_test_irq_handler_cap = dummy_irq;
    oz::sys_tcb_configure(ithread, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_irq_replenish_handler_entry), istack_top);

    std::uint32_t ibaton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    std::uint32_t inotif = oz::sys_notification_create(oz::CAP_UNTYPED);
    static constexpr std::uint64_t IRQ_BADGE = (1ULL << 12);
    oz::sys_baton_bind_notification(ibaton, inotif, IRQ_BADGE);
    oz::sys_tcb_bind_baton(ithread, ibaton);

    g_irq_replenish_step = 0;
    oz::sys_baton_inject(ibaton, 50000);
    oz::SchedResult ires = oz::sys_baton_switch(ibaton);
    bool irq_resv_ok = (g_irq_replenish_step == 1);
    TEST_REPORT("KTEST", "InterruptThread sys_irq_wait reservation", irq_resv_ok);

    oz::sys_baton_inject(ibaton, 50000);
    oz::sys_tcb_resume(ithread);
    ires = oz::sys_baton_switch(ibaton);
    bool irq_resume_ok = (g_irq_replenish_step == 2);
    TEST_REPORT("KTEST", "InterruptThread resume and clean return", irq_resume_ok);

    oz::sys_baton_inject(ibaton, 0);
    ires = oz::sys_baton_switch(ibaton);
    bool irq_timeout_ok = (ires.reason == oz::SYS_BATON_SWITCH_TIMEOUT);
    TEST_REPORT("KTEST", "sys_baton_switch timeout on exhausted handler", irq_timeout_ok);
}

} // namespace oz::ktest

extern "C" void _start() {
    oz::sys_debug_print("\n\r========================================================\n\r");
    oz::sys_debug_print(" [OZ_OS] Standalone Kernel Test Runner (ktest)\n\r");
    oz::sys_debug_print("========================================================\n\r");

    oz::ktest::run_capability_semantics_tests();
    oz::ktest::run_baton_scheduling_tests();

    oz::test::print_summary("KTEST");
    oz::test::exit_qemu();
}
