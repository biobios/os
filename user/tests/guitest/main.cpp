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
    bool ok = oz::sys_call(cap_display_send, &req_surf, &resp_surf) && resp_surf.data[0] == 0 && resp_surf.has_cap;
    TEST_REPORT("GUITEST", "Display create_surface (FrameCap Transfer)", ok);

    if (ok) {
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
    bool ok1 = oz::sys_call(cap_wm_send, &req_win1, &resp_win1) && resp_win1.data[0] == 0 && resp_win1.has_cap;
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
        bool ok2 = oz::sys_call(cap_wm_send, &req_win2, &resp_win2) && resp_win2.data[0] == 0 && resp_win2.has_cap;
        TEST_REPORT("GUITEST", "WM create_window 2 (BSP Auto-Split)", ok2);
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
    oz::sys_cspace_cap_insert(disp_proc.cspace_cap, 3, disp_cmd_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(disp_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
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

    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, oz::guitest::CAP_DISPLAY_SEND, disp_cmd_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));

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

    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 0, wm_proc.vspace_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 2, disp_cmd_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 3, wm_disp_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 4, wm_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 5, kbd_ring_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 6, notif_wm_kbd, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));

    oz::sys_tcb_resume(wm_proc.tcb_cap);
    init_coroutine(wm_proc.tcb_cap);

    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, oz::guitest::CAP_WM_SEND, wm_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));

    // Run Tests
    oz::guitest::run_display_tests(oz::guitest::CAP_DISPLAY_SEND);
    oz::guitest::run_wm_tests(oz::guitest::CAP_WM_SEND);

    oz::test::print_summary("GUITEST");
    oz::test::exit_qemu();
}
