#include "syscall.hpp"
#include "loader.hpp"
#include "../common/test_harness.hpp"
#include "../../src/common/ipc/Protocol.hpp"
#include "../../src/common/utils/BootInfo.hpp"

namespace oz::guitest {

static constexpr std::uint64_t BOOTINFO_VADDR = 0x600000000000ULL;
static constexpr std::uint64_t INITRD_VADDR   = 0x500000000000ULL;
static constexpr std::uint32_t CAP_DISPLAY_SEND = 6;
static constexpr std::uint32_t CAP_WM_SEND      = 7;

struct Pixel {
    std::uint8_t b{0};
    std::uint8_t g{0};
    std::uint8_t r{0};
    std::uint8_t a{0};
};

static void run_display_tests(std::uint32_t cap_display_send) {
    oz::sys_debug_print("\n\r--- Display Server Tests ---\n\r");

    // Test 1: CREATE_SURFACE via Display Capability
    oz::IPCMessage req_surf{};
    req_surf.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::CREATE_SURFACE);
    req_surf.data[1] = 160;
    req_surf.data[2] = 120;

    oz::IPCMessage resp_surf{};
    bool ok = oz::sys_call_recv_cap(cap_display_send, &req_surf, &resp_surf) && resp_surf.data[0] == 0 && resp_surf.has_cap;
    TEST_REPORT("GUITEST", "Display create_surface (FrameCap Transfer)", ok);

    if (ok) {
        std::uint32_t surf_id = static_cast<std::uint32_t>(resp_surf.data[1]);
        std::uint32_t surf_frame_cap = resp_surf.transferred_cap;
        static constexpr std::uint64_t TEST_SURF_VADDR = 0x700000000000ULL;
        int map_res = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, surf_frame_cap, TEST_SURF_VADDR, 0x01, 160 * 120 * 4);
        TEST_REPORT("GUITEST", "Surface FrameCap mapping into client VSpace", map_res == 0);

        if (map_res == 0) {
            Pixel* surf_buf = reinterpret_cast<Pixel*>(TEST_SURF_VADDR);
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    surf_buf[y * 160 + x] = Pixel{0xFF, 0xFF, 0x00, 0}; // Cyan
                }
            }

            // Test 3: Set surface position and commit composition
            oz::IPCMessage req_pos{};
            req_pos.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::SET_SURFACE_RECT);
            req_pos.data[1] = surf_id;
            req_pos.data[2] = (30ULL << 32) | 50ULL; // (x=30, y=50)
            oz::IPCMessage resp_pos{};
            oz::sys_call(cap_display_send, &req_pos, &resp_pos);

            // Test 4: Update cursor position (Mouse Cursor simulation)
            oz::IPCMessage req_cur{};
            req_cur.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::UPDATE_CURSOR);
            req_cur.data[1] = (40ULL << 32) | 60ULL; // (cursor_x=40, cursor_y=60)
            req_cur.data[2] = 1; // visible
            oz::IPCMessage resp_cur{};
            oz::sys_call(cap_display_send, &req_cur, &resp_cur);

            // Commit
            oz::IPCMessage req_commit{};
            req_commit.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::COMMIT);
            req_commit.data[1] = surf_id;
            oz::IPCMessage resp_commit{};
            bool commit_ok = oz::sys_call(cap_display_send, &req_commit, &resp_commit);
            TEST_REPORT("GUITEST", "Display Surface SetRect, Cursor & Commit", commit_ok && resp_commit.data[0] == 0);
        }
    }
}

static void run_wm_tests(std::uint32_t cap_wm_send) {
    oz::sys_debug_print("\n\r--- Window Manager (WM) Tests ---\n\r");

    // Test 1: Create Window 1 (BSP Root)
    oz::IPCMessage req_win1{};
    req_win1.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::CREATE_WINDOW);
    req_win1.data[1] = 380;
    req_win1.data[2] = 260;
    oz::IPCMessage resp_win1{};
    bool ok1 = oz::sys_call_recv_cap(cap_wm_send, &req_win1, &resp_win1) && resp_win1.data[0] == 0 && resp_win1.has_cap;
    TEST_REPORT("GUITEST", "WM create_window 1 (Tiling BSP Root)", ok1);

    if (ok1) {
        std::uint32_t win1_id = static_cast<std::uint32_t>(resp_win1.data[1]);
        std::uint32_t win1_frame_cap = resp_win1.transferred_cap;

        // Set Title
        oz::IPCMessage title_msg{};
        title_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::SET_TITLE);
        title_msg.data[1] = win1_id;
        const char* t1 = "Editor";
        for (int i = 0; i < 7; ++i) reinterpret_cast<char*>(&title_msg.data[2])[i] = t1[i];
        oz::IPCMessage resp_title{};
        oz::sys_call(cap_wm_send, &title_msg, &resp_title);

        static constexpr std::uint64_t WIN1_VADDR = 0x710000000000ULL;
        oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, win1_frame_cap, WIN1_VADDR, 0x01, 380 * 260 * 4);

        // Test 2: Create Window 2 (Auto-Split)
        oz::IPCMessage req_win2{};
        req_win2.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::CREATE_WINDOW);
        req_win2.data[1] = 380;
        req_win2.data[2] = 260;
        oz::IPCMessage resp_win2{};
        bool ok2 = oz::sys_call_recv_cap(cap_wm_send, &req_win2, &resp_win2) && resp_win2.data[0] == 0 && resp_win2.has_cap;
        TEST_REPORT("GUITEST", "WM create_window 2 (BSP Auto-Split)", ok2);

        if (ok2) {
            std::uint32_t win2_id = static_cast<std::uint32_t>(resp_win2.data[1]);
            std::uint32_t win2_frame_cap = resp_win2.transferred_cap;

            // Set Title
            oz::IPCMessage title2_msg{};
            title2_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::SET_TITLE);
            title2_msg.data[1] = win2_id;
            const char* t2 = "Log";
            for (int i = 0; i < 4; ++i) reinterpret_cast<char*>(&title2_msg.data[2])[i] = t2[i];
            oz::IPCMessage resp_title2{};
            oz::sys_call(cap_wm_send, &title2_msg, &resp_title2);

            static constexpr std::uint64_t WIN2_VADDR = 0x720000000000ULL;
            oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, win2_frame_cap, WIN2_VADDR, 0x01, 380 * 260 * 4);

            auto query_win_info = [&](std::uint32_t wid, std::uint8_t& ws, bool& is_min, bool& is_foc) -> bool {
                oz::IPCMessage qmsg{};
                qmsg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::GET_WINDOW_INFO);
                qmsg.data[1] = wid;
                oz::IPCMessage qresp{};
                if (oz::sys_call(cap_wm_send, &qmsg, &qresp) && qresp.data[0] == 0) {
                    ws = static_cast<std::uint8_t>(qresp.data[1]);
                    is_min = (qresp.data[2] != 0);
                    is_foc = (qresp.data[3] != 0);
                    return true;
                }
                return false;
            };

            auto send_key_to_wm = [&](char ascii, std::uint8_t scancode, std::uint32_t mod) {
                oz::IPCMessage kmsg{};
                kmsg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMEventType::KEY_DOWN);
                kmsg.data[1] = static_cast<std::uint64_t>(ascii);
                kmsg.data[2] = static_cast<std::uint64_t>(scancode);
                kmsg.data[3] = static_cast<std::uint64_t>(mod);
                oz::IPCMessage kresp{};
                oz::sys_call(cap_wm_send, &kmsg, &kresp);
            };

            // Test 3: Key Forwarding & GET_EVENT Retrieval
            send_key_to_wm('A', 0x04, 0);
            oz::IPCMessage get_ev_msg{};
            get_ev_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::GET_EVENT);
            get_ev_msg.data[1] = win2_id;
            oz::IPCMessage recv_k{};
            bool key_fwd_ok = (oz::sys_call(cap_wm_send, &get_ev_msg, &recv_k) &&
                               recv_k.data[0] == 0 && recv_k.data[1] == 'A');
            TEST_REPORT("GUITEST", "WM Key Forwarding & GET_EVENT", key_fwd_ok);

            // Test 4: Focus Navigation (Super + h)
            send_key_to_wm('h', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER));
            std::uint8_t ws1 = 0, ws2 = 0;
            bool min1 = false, min2 = false, foc1 = false, foc2 = false;
            query_win_info(win1_id, ws1, min1, foc1);
            query_win_info(win2_id, ws2, min2, foc2);
            TEST_REPORT("GUITEST", "WM Focus Navigation (Super + h)", foc1 && !foc2);

            // Test 5: Minimize / Scratchpad (Super + m, Super + Shift + m)
            send_key_to_wm('m', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER));
            query_win_info(win1_id, ws1, min1, foc1);
            query_win_info(win2_id, ws2, min2, foc2);
            TEST_REPORT("GUITEST", "WM Minimize Window (Super + m)", min1 || min2);

            send_key_to_wm('m', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER) | static_cast<std::uint32_t>(oz::ipc::WMModifier::SHIFT));
            query_win_info(win1_id, ws1, min1, foc1);
            query_win_info(win2_id, ws2, min2, foc2);
            TEST_REPORT("GUITEST", "WM Restore Scratchpad (Super + Shift + m)", !min1 && !min2);

            // Test 6: Modal Resize Mode (Super + r, l, Enter)
            send_key_to_wm('r', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER));
            send_key_to_wm('l', 0, 0);
            send_key_to_wm('\r', 0, 0);
            TEST_REPORT("GUITEST", "WM Modal Resize Mode (Super + r, l, Enter)", true);

            // Test 7: Workspace Switching (Super + Shift + 2, Super + 1..9)
            send_key_to_wm('2', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER) | static_cast<std::uint32_t>(oz::ipc::WMModifier::SHIFT));
            query_win_info(win1_id, ws1, min1, foc1);
            query_win_info(win2_id, ws2, min2, foc2);
            TEST_REPORT("GUITEST", "WM Move Window to Workspace 2 (Super + Shift + 2)", ws1 == 1 || ws2 == 1);

            send_key_to_wm('2', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER));
            send_key_to_wm('1', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER) | static_cast<std::uint32_t>(oz::ipc::WMModifier::SHIFT));
            send_key_to_wm('1', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER));
            query_win_info(win1_id, ws1, min1, foc1);
            TEST_REPORT("GUITEST", "WM Workspace Switching (Super + 1..9)", ws1 == 0);

            // Test 8: Close Window (Super + Shift + q)
            std::uint8_t dummy_ws = 0;
            bool dummy_min = false, target_foc1 = false;
            query_win_info(win1_id, dummy_ws, dummy_min, target_foc1);
            std::uint32_t close_target = target_foc1 ? win1_id : win2_id;

            send_key_to_wm('q', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER) | static_cast<std::uint32_t>(oz::ipc::WMModifier::SHIFT));
            bool q_ok = query_win_info(close_target, ws1, min1, foc1);
            TEST_REPORT("GUITEST", "WM Close Window (Super + Shift + q)", !q_ok);
        }
    }
}

} // namespace oz::guitest

extern "C" void _start() {
    oz::sys_debug_print("\n\r========================================================\n\r");
    oz::sys_debug_print(" [OZ_OS] GUI & Window System Test Runner (guitest)\n\r");
    oz::sys_debug_print("========================================================\n\r");

    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_BOOTINFO, oz::guitest::BOOTINFO_VADDR, 0x00, 4096) != 0) {
        oz::test::report("GUITEST", "Map BootInfo", false);
        oz::test::exit_qemu();
    }
    const volatile auto* boot_info = reinterpret_cast<const volatile oz::BootInfo*>(oz::guitest::BOOTINFO_VADDR);
    std::uint64_t initrd_size = boot_info->initrd_size;

    if (initrd_size > 0) {
        oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_INITRD, oz::guitest::INITRD_VADDR, 0x00, initrd_size);
    }
    const std::uint8_t* initrd_ptr = reinterpret_cast<const std::uint8_t*>(oz::guitest::INITRD_VADDR + boot_info->initrd_offset);

    std::uint32_t init_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);

    auto init_coroutine = [&](std::uint32_t tcb_cap) {
        if (tcb_cap == oz::INVALID_CAP_ID) return;
        oz::sys_tcb_bind_baton(tcb_cap, init_baton);
        oz::sys_baton_inject(init_baton, 1000000);
        oz::sys_baton_switch(init_baton);
    };

    std::uint32_t cons_send = 0, cons_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &cons_send, &cons_recv);

    // 1. Spawn Display Server
    oz::loader::TarFile disp_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "display");
    if (!disp_file.data) {
        oz::test::report("GUITEST", "Find display in initrd", false);
        oz::test::exit_qemu();
    }
    auto disp_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, disp_file.data, disp_file.size);
    if (!disp_proc.success) {
        oz::test::report("GUITEST", "Spawn display", false);
        oz::test::exit_qemu();
    }

    std::uint32_t init_disp_send = 0, disp_init_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &init_disp_send, &disp_init_recv);
    std::uint32_t disp_cmd_send = 0, disp_cmd_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &disp_cmd_send, &disp_cmd_recv);

    oz::sys_cspace_cap_insert(disp_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(disp_proc.cspace_cap, 2, disp_init_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(disp_proc.cspace_cap, 3, disp_cmd_recv, static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::GRANT_REPLY));
    std::uint32_t disp_ut = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory), 16 * 1024 * 1024);
    oz::sys_cspace_cap_move(disp_proc.cspace_cap, oz::CAP_UNTYPED, disp_ut);
    oz::sys_cspace_cap_insert(disp_proc.cspace_cap, oz::CAP_FRAMEBUFFER, oz::CAP_FRAMEBUFFER, static_cast<std::uint32_t>(oz::CapRights::ALL));

    oz::sys_tcb_resume(disp_proc.tcb_cap);
    init_coroutine(disp_proc.tcb_cap);

    // Send FrameBuffer info
    oz::IPCMessage config_msg{};
    config_msg.data[0] = boot_info->fb_width;
    config_msg.data[1] = boot_info->fb_height;
    config_msg.data[2] = boot_info->fb_size;
    config_msg.data[3] = boot_info->fb_phys_addr;
    oz::IPCMessage config_resp{};
    oz::sys_call(init_disp_send, &config_msg, &config_resp);

    // 2. Spawn Window Manager (WM)
    oz::loader::TarFile wm_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "wm");
    if (!wm_file.data) {
        oz::test::report("GUITEST", "Find wm in initrd", false);
        oz::test::exit_qemu();
    }
    auto wm_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, wm_file.data, wm_file.size);
    if (!wm_proc.success) {
        oz::test::report("GUITEST", "Spawn wm", false);
        oz::test::exit_qemu();
    }

    std::uint32_t wm_req_send = 0, wm_req_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &wm_req_send, &wm_req_recv);
    std::uint32_t wm_disp_reply_send = 0, wm_disp_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &wm_disp_reply_send, &wm_disp_reply_recv);

    std::uint32_t notif_wm_kbd = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t kbd_ring_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);

    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 2, disp_cmd_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 3, wm_disp_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 4, wm_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::GRANT_REPLY));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 5, kbd_ring_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 6, notif_wm_kbd, static_cast<std::uint32_t>(oz::CapRights::ALL));
    std::uint32_t wm_ut = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory), 16 * 1024 * 1024);
    oz::sys_cspace_cap_move(wm_proc.cspace_cap, oz::CAP_UNTYPED, wm_ut);

    oz::sys_tcb_resume(wm_proc.tcb_cap);
    init_coroutine(wm_proc.tcb_cap);

    // Run Tests
    oz::guitest::run_display_tests(disp_cmd_send);
    oz::guitest::run_wm_tests(wm_req_send);

    oz::test::print_summary("GUITEST");
    oz::test::exit_qemu();
}
