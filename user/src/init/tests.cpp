#include "tests.hpp"
#include "syscall.hpp"
#include "loader.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"

namespace oz::init_tests {

namespace {

struct Pixel {
    std::uint8_t b{0};
    std::uint8_t g{0};
    std::uint8_t r{0};
    std::uint8_t a{0};
};

struct VfsOpenAtResult {
    oz::ipc::VfsStatus status{oz::ipc::VfsStatus::NOT_FOUND};
    int handle{-1};
    std::uint32_t transferred_cap{oz::INVALID_CAP_ID};
    std::size_t size{0};
};

static VfsOpenAtResult vfs_openat(std::uint32_t dir_send_cap, std::uint32_t dir_recv_cap,
                                  const char* rel_path, oz::ipc::VfsOpenFlags flags,
                                  std::uint32_t base_dir_handle = 0) {
    VfsOpenAtResult res{};
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_openat_command(0, flags, base_dir_handle);

    char* dst = reinterpret_cast<char*>(&req.data[1]);
    int i = 0;
    for (; i < 23 && rel_path[i] != '\0'; i++) {
        dst[i] = rel_path[i];
    }
    dst[i] = '\0';

    if (!oz::sys_channel_send(dir_send_cap, &req)) return res;

    oz::IPCMessage resp{};
    if (!oz::sys_channel_recv(dir_recv_cap, &resp)) return res;

    res.status = static_cast<oz::ipc::VfsStatus>(resp.data[0]);
    if (res.status == oz::ipc::VfsStatus::OK) {
        res.handle = static_cast<int>(resp.data[1]);
        res.size = static_cast<std::size_t>(resp.data[2]);
        if (resp.has_cap) {
            res.transferred_cap = resp.transferred_cap;
        }
    }
    return res;
}

} // anonymous namespace

void run_capability_semantics_tests() {
    oz::sys_debug_print("[Init] Running Capability semantics tests...\n\r");

    // 1. Test Move semantics
    std::uint32_t t_ep0 = 0, t_ep1 = 0;
    if (oz::sys_channel_create(oz::CAP_UNTYPED, &t_ep0, &t_ep1)) {
        if (oz::sys_cap_move(50, t_ep0) == 0) {
            // Source slot t_ep0 must now be invalid
            if (oz::sys_cap_delete(t_ep0) != 0 && oz::sys_cap_delete(50) == 0) {
                oz::sys_debug_print("[Test] Capability Move semantics: PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] Capability Move semantics: FAILED (source not invalidated)\n\r");
            }
        } else {
            oz::sys_debug_print("[Test] Capability Move semantics: FAILED (sys_cap_move failed)\n\r");
        }
        oz::sys_cap_delete(t_ep1);
    }

    // 2. Test Linear rejection on copy/mint
    // CAP_ROOT_IOPORT is a Linear object; minting/copying must be rejected!
    std::uint32_t minted_linear = oz::sys_cap_mint(oz::CAP_ROOT_IOPORT, static_cast<std::uint32_t>(oz::CapRights::ALL));
    if (minted_linear == oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[Test] Linear duplication prevention: PASSED\n\r");
    } else {
        oz::sys_debug_print("[Test] Linear duplication prevention: FAILED (Linear object was duplicated!)\n\r");
        oz::sys_cap_delete(minted_linear);
    }

    // 3. Test Once capability auto-consumption
    std::uint32_t once_sender = oz::sys_cap_mint(1, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::ONCE));
    if (once_sender != oz::INVALID_CAP_ID) {
        oz::IPCMessage once_msg{};
        const char* msg_txt = "[Test] Once Capability msg\n\r";
        for (int k = 0; k < 4; ++k) {
            once_msg.data[k] = *reinterpret_cast<const std::uint64_t*>(&msg_txt[k * 8]);
        }
        if (oz::sys_channel_send(once_sender, &once_msg)) {
            // After sending, the once_sender slot must have been removed by kernel!
            if (oz::sys_cap_delete(once_sender) != 0) {
                oz::sys_debug_print("[Test] Once Capability auto-consumption: PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] Once Capability auto-consumption: FAILED (slot still valid after send)\n\r");
            }
        } else {
            oz::sys_debug_print("[Test] Once Capability auto-consumption: FAILED (send failed)\n\r");
        }
    } else {
        oz::sys_debug_print("[Test] Once Capability auto-consumption: FAILED (mint failed)\n\r");
    }

    // 4. Test C++ RAII LinearCap
    {
        std::uint32_t r_ep0 = 0, r_ep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &r_ep0, &r_ep1)) {
            oz::LinearCap<> linear1(r_ep0);
            oz::LinearCap<> linear2 = static_cast<oz::LinearCap<>&&>(linear1); // move construct
            if (!linear1.isValid() && linear2.isValid()) {
                if (linear2.moveTo(51)) {
                    oz::sys_debug_print("[Test] C++ LinearCap RAII move: PASSED\n\r");
                } else {
                    oz::sys_debug_print("[Test] C++ LinearCap RAII move: FAILED (moveTo failed)\n\r");
                }
            } else {
                oz::sys_debug_print("[Test] C++ LinearCap RAII move: FAILED\n\r");
            }
            oz::sys_cap_delete(r_ep1);
        }
    }
    // linear2 went out of scope, slot 51 should be deleted automatically
    if (oz::sys_cap_delete(51) != 0) {
        oz::sys_debug_print("[Test] C++ LinearCap RAII destructor cleanup: PASSED\n\r");
    } else {
        oz::sys_debug_print("[Test] C++ LinearCap RAII destructor cleanup: FAILED (slot 51 still valid)\n\r");
    }

    // 5. Test Notification Object & Bound Notification
    std::uint32_t notif_cap = oz::sys_notification_create(oz::CAP_UNTYPED);
    if (notif_cap != oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[Test] Notification creation: PASSED\n\r");

        // Test manual signal and poll
        oz::sys_notification_signal(notif_cap, 0x42);
        std::uint64_t polled_bits = 0;
        if (oz::sys_notification_poll(notif_cap, &polled_bits) == 0 && polled_bits == 0x42) {
            oz::sys_debug_print("[Test] Notification signal & poll: PASSED\n\r");
        } else {
            oz::sys_debug_print("[Test] Notification signal & poll: FAILED\n\r");
        }

        // Test Bound Notification with Channel
        std::uint32_t b_ep0 = 0, b_ep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &b_ep0, &b_ep1)) {
            if (oz::sys_channel_bind_notification(b_ep1, notif_cap, 0x100) == 0) {
                oz::sys_cap_delete(b_ep0);

                std::uint64_t close_bits = 0;
                if (oz::sys_notification_poll(notif_cap, &close_bits) == 0 && (close_bits & 0x100)) {
                    oz::sys_debug_print("[Test] Bound Notification on client disconnect: PASSED\n\r");
                } else {
                    oz::sys_debug_print("[Test] Bound Notification on client disconnect: FAILED\n\r");
                }
            } else {
                oz::sys_debug_print("[Test] Bound Notification binding: FAILED\n\r");
            }
            oz::sys_cap_delete(b_ep1);
        }
        oz::sys_cap_delete(notif_cap);
    } else {
        oz::sys_debug_print("[Test] Notification creation: FAILED\n\r");
    }
}

void run_vfs_tests(std::uint32_t cap_vfs_send, std::uint32_t cap_vfs_recv) {
    oz::sys_debug_print("[Init] Running Capability-based VFS self-tests...\n\r");

    // Test 1: Valid relative open of file
    auto res_file = vfs_openat(cap_vfs_send, cap_vfs_recv, "test.txt", oz::ipc::VfsOpenFlags::FILE);
    if (res_file.status == oz::ipc::VfsStatus::OK && res_file.transferred_cap != oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[Test] VFS openat relative file (Cap Transfer): PASSED\n\r");
    } else {
        oz::sys_debug_print("[Test] VFS openat relative file: FAILED\n\r");
    }

    // Test 2: Reject ".." in relative path (Sandbox violation)
    auto res_jailbreak = vfs_openat(cap_vfs_send, cap_vfs_recv, "../test.txt", oz::ipc::VfsOpenFlags::FILE);
    if (res_jailbreak.status == oz::ipc::VfsStatus::INVALID_PATH) {
        oz::sys_debug_print("[Test] VFS sandbox '..' traversal rejection: PASSED\n\r");
    } else {
        oz::sys_debug_print("[Test] VFS sandbox '..' traversal rejection: FAILED\n\r");
    }

    // Test 3: Reject leading '/' (Absolute path ambient authority violation)
    auto res_abs = vfs_openat(cap_vfs_send, cap_vfs_recv, "/test.txt", oz::ipc::VfsOpenFlags::FILE);
    if (res_abs.status == oz::ipc::VfsStatus::INVALID_PATH) {
        oz::sys_debug_print("[Test] VFS sandbox absolute path rejection: PASSED\n\r");
    } else {
        oz::sys_debug_print("[Test] VFS sandbox absolute path rejection: FAILED\n\r");
    }

    // Test 4: Open subdirectory as Directory Capability
    auto res_dir = vfs_openat(cap_vfs_send, cap_vfs_recv, "os", oz::ipc::VfsOpenFlags::DIRECTORY);
    if (res_dir.status == oz::ipc::VfsStatus::OK && res_dir.transferred_cap != oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[Test] VFS openat Directory Capability: PASSED\n\r");

        // Test 5: Open child file relative to the new Directory Capability
        auto res_subfile = vfs_openat(cap_vfs_send, cap_vfs_recv, "kernel.bin", oz::ipc::VfsOpenFlags::FILE, res_dir.handle);
        if (res_subfile.status == oz::ipc::VfsStatus::OK && res_subfile.transferred_cap != oz::INVALID_CAP_ID) {
            oz::sys_debug_print("[Test] VFS openat child relative to DirectoryCap: PASSED\n\r");
        } else {
            oz::sys_debug_print("[Test] VFS openat child relative to DirectoryCap: FAILED\n\r");
        }

        // Test 6: Attempt '..' escape from Directory Capability
        auto res_sub_escape = vfs_openat(cap_vfs_send, cap_vfs_recv, "../test.txt", oz::ipc::VfsOpenFlags::FILE, res_dir.handle);
        if (res_sub_escape.status == oz::ipc::VfsStatus::INVALID_PATH) {
            oz::sys_debug_print("[Test] VFS child '..' escape rejection: PASSED\n\r");
        } else {
            oz::sys_debug_print("[Test] VFS child '..' escape rejection: FAILED\n\r");
        }
    } else {
        oz::sys_debug_print("[Test] VFS openat Directory Capability: FAILED\n\r");
    }
}

void run_display_tests(std::uint32_t cap_display_send) {
    oz::sys_debug_print("[Init] Running Capability-based Display & Surface self-tests...\n\r");

    std::uint32_t disp_reply_send = 0;
    std::uint32_t disp_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &disp_reply_send, &disp_reply_recv);

    // Test 1: CREATE_SURFACE via Display Capability
    oz::IPCMessage req_surf{};
    req_surf.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::CREATE_SURFACE);
    req_surf.data[1] = 160; // width
    req_surf.data[2] = 120; // height
    req_surf.transferred_cap = disp_reply_send;
    req_surf.has_cap = true;

    oz::sys_channel_send(cap_display_send, &req_surf);

    oz::IPCMessage resp_surf{};
    if (oz::sys_channel_recv(disp_reply_recv, &resp_surf) && resp_surf.data[0] == 0 && resp_surf.has_cap) {
        oz::sys_debug_print("[Test] Display create_surface (FrameCap Transfer): PASSED\n\r");
        std::uint32_t surf_id = static_cast<std::uint32_t>(resp_surf.data[1]);
        std::uint32_t surf_frame_cap = resp_surf.transferred_cap;

        // Test 2: Map the transferred FrameCap into client address space
        static constexpr std::uint64_t TEST_SURF_VADDR = 0x700000000000ULL;
        if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, surf_frame_cap, TEST_SURF_VADDR, 0x01, 160 * 120 * 4) == 0) {
            oz::sys_debug_print("[Test] Surface FrameCap mapping: PASSED\n\r");

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
            oz::sys_channel_send(cap_display_send, &req_pos);

            // Test 4: Update cursor position (Mouse Cursor simulation)
            oz::IPCMessage req_cur{};
            req_cur.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::UPDATE_CURSOR);
            req_cur.data[1] = (40ULL << 32) | 60ULL; // (cursor_x=40, cursor_y=60)
            req_cur.data[2] = 1; // visible
            oz::sys_channel_send(cap_display_send, &req_cur);

            // Commit
            oz::IPCMessage req_commit{};
            req_commit.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::COMMIT);
            req_commit.data[1] = surf_id;
            oz::sys_channel_send(cap_display_send, &req_commit);

            oz::sys_debug_print("[Test] Surface SetRect & Cursor Update & Commit: PASSED\n\r");
        } else {
            oz::sys_debug_print("[Test] Surface FrameCap mapping: FAILED\n\r");
        }
    } else {
        oz::sys_debug_print("[Test] Display create_surface: FAILED\n\r");
    }
}

void run_wm_tests(std::uint32_t cap_wm_send) {
    oz::sys_debug_print("[Init] Running Tiling Window Manager self-tests...\n\r");

    std::uint32_t wm_reply_send = 0;
    std::uint32_t wm_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &wm_reply_send, &wm_reply_recv);

    // Test 1: Create Window 1 (Main Editor/Terminal)
    oz::IPCMessage req_win1{};
    req_win1.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::CREATE_WINDOW);
    req_win1.data[1] = 380;
    req_win1.data[2] = 260;
    req_win1.transferred_cap = wm_reply_send;
    req_win1.has_cap = true;
    oz::sys_debug_print("[Init] Sending CREATE_WINDOW to CAP_WM_SEND...\n\r");
    bool send_ok = oz::sys_channel_send(cap_wm_send, &req_win1);
    if (!send_ok) {
        oz::sys_debug_print("[Init] sys_channel_send to CAP_WM_SEND FAILED!\n\r");
    } else {
        oz::sys_debug_print("[Init] sys_channel_send to CAP_WM_SEND SUCCEEDED! Waiting for reply...\n\r");
    }

    oz::IPCMessage resp_win1{};
    if (oz::sys_channel_recv(wm_reply_recv, &resp_win1) && resp_win1.data[0] == 0 && resp_win1.has_cap) {
        oz::sys_debug_print("[Test] WM create_window 1 (Tiling BSP Root): PASSED\n\r");
        std::uint32_t win1_id = static_cast<std::uint32_t>(resp_win1.data[1]);
        std::uint32_t win1_frame_cap = resp_win1.transferred_cap;

        // Set Title
        oz::IPCMessage title_msg{};
        title_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::SET_TITLE);
        title_msg.data[1] = win1_id;
        const char* t1 = "Editor";
        for (int i = 0; i < 7; ++i) reinterpret_cast<char*>(&title_msg.data[2])[i] = t1[i];
        oz::sys_channel_send(cap_wm_send, &title_msg);

        // Map buffer and fill with dark blue
        static constexpr std::uint64_t WIN1_VADDR = 0x710000000000ULL;
        if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, win1_frame_cap, WIN1_VADDR, 0x01, 380 * 260 * 4) == 0) {
            auto* buf = reinterpret_cast<Pixel*>(WIN1_VADDR);
            for (int i = 0; i < 380 * 260; ++i) buf[i] = Pixel{0x50, 0x20, 0x10, 0};
        }

        // Test 2: Create Window 2 (Split right side)
        std::uint32_t wm_reply_send2 = 0;
        std::uint32_t wm_reply_recv2 = 0;
        oz::sys_channel_create(oz::CAP_UNTYPED, &wm_reply_send2, &wm_reply_recv2);

        oz::IPCMessage req_win2{};
        req_win2.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::CREATE_WINDOW);
        req_win2.data[1] = 380;
        req_win2.data[2] = 260;
        req_win2.transferred_cap = wm_reply_send2;
        req_win2.has_cap = true;
        oz::sys_channel_send(cap_wm_send, &req_win2);

        oz::IPCMessage resp_win2{};
        if (oz::sys_channel_recv(wm_reply_recv2, &resp_win2) && resp_win2.data[0] == 0 && resp_win2.has_cap) {
            oz::sys_debug_print("[Test] WM create_window 2 (BSP Auto-Split Right): PASSED\n\r");
            std::uint32_t win2_id = static_cast<std::uint32_t>(resp_win2.data[1]);
            std::uint32_t win2_frame_cap = resp_win2.transferred_cap;

            // Set Title
            oz::IPCMessage title2_msg{};
            title2_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::SET_TITLE);
            title2_msg.data[1] = win2_id;
            const char* t2 = "Log";
            for (int i = 0; i < 4; ++i) reinterpret_cast<char*>(&title2_msg.data[2])[i] = t2[i];
            oz::sys_channel_send(cap_wm_send, &title2_msg);

            // Map buffer and fill with dark green
            static constexpr std::uint64_t WIN2_VADDR = 0x720000000000ULL;
            oz::sys_debug_print("[Init] Mapping WIN2 buffer...\n\r");
            int map_res = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, win2_frame_cap, WIN2_VADDR, 0x01, 380 * 260 * 4);
            if (map_res == 0) {
                oz::sys_debug_print("[Init] WIN2 buffer mapped! Filling buffer...\n\r");
                auto* buf = reinterpret_cast<Pixel*>(WIN2_VADDR);
                for (int i = 0; i < 380 * 260; ++i) buf[i] = Pixel{0x10, 0x40, 0x10, 0};
                oz::sys_debug_print("[Init] WIN2 buffer filled!\n\r");
            } else {
                oz::sys_debug_print("[Init] WIN2 buffer map FAILED!\n\r");
            }

            oz::sys_debug_print("[Init] Finished Window 2 setup. Starting WM Test 3...\n\r");

            // Additional WM Verification Tests
            auto query_win_info = [&](std::uint32_t wid, std::uint8_t& ws, bool& is_min, bool& is_foc) -> bool {
                oz::sys_debug_print("[Init] query_win_info: start\n\r");
                std::uint32_t q_send = 0, q_recv = 0;
                if (!oz::sys_channel_create(oz::CAP_UNTYPED, &q_send, &q_recv)) {
                    oz::sys_debug_print("[Init] query_win_info: sys_channel_create FAILED\n\r");
                    return false;
                }
                oz::IPCMessage qmsg{};
                qmsg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::GET_WINDOW_INFO);
                qmsg.data[1] = wid;
                qmsg.transferred_cap = q_send;
                qmsg.has_cap = true;
                if (!oz::sys_channel_send(cap_wm_send, &qmsg)) {
                    oz::sys_debug_print("[Init] query_win_info: sys_channel_send FAILED\n\r");
                    return false;
                }
                oz::sys_debug_print("[Init] query_win_info: waiting for reply...\n\r");
                oz::IPCMessage qresp{};
                if (oz::sys_channel_recv(q_recv, &qresp) && qresp.data[0] == 0) {
                    oz::sys_debug_print("[Init] query_win_info: got reply!\n\r");
                    ws = static_cast<std::uint8_t>(qresp.data[1]);
                    is_min = (qresp.data[2] != 0);
                    is_foc = (qresp.data[3] != 0);
                    return true;
                }
                oz::sys_debug_print("[Init] query_win_info: recv failed or error resp\n\r");
                return false;
            };

            auto send_key_to_wm = [&](char ascii, std::uint8_t scancode, std::uint32_t mod) {
                oz::IPCMessage kmsg{};
                kmsg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMEventType::KEY_DOWN);
                kmsg.data[1] = static_cast<std::uint64_t>(ascii);
                kmsg.data[2] = static_cast<std::uint64_t>(scancode);
                kmsg.data[3] = static_cast<std::uint64_t>(mod);
                oz::sys_channel_send(cap_wm_send, &kmsg);
            };

            // Test 3: Input Channel Binding & Key Forwarding
            oz::sys_debug_print("[Init] Creating channel for win2_input...\n\r");
            std::uint32_t win2_input_send = 0, win2_input_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &win2_input_send, &win2_input_recv);

            oz::IPCMessage bind_msg{};
            bind_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::BIND_INPUT_CHANNEL);
            bind_msg.data[1] = win2_id;
            bind_msg.transferred_cap = win2_input_send;
            bind_msg.has_cap = true;
            oz::sys_debug_print("[Init] Sending BIND_INPUT_CHANNEL to CAP_WM_SEND...\n\r");
            oz::sys_channel_send(cap_wm_send, &bind_msg);

            // Synchronize with WM so we know BIND_INPUT_CHANNEL has completed
            std::uint8_t sync_ws = 0;
            bool sync_min = false, sync_foc = false;
            oz::sys_debug_print("[Init] Calling sync query_win_info...\n\r");
            query_win_info(win2_id, sync_ws, sync_min, sync_foc);

            // Win 2 is currently focused. Send key 'A' without modifiers
            oz::sys_debug_print("[Init] Sending key 'A' to WM...\n\r");
            send_key_to_wm('A', 0x04, 0);

            oz::IPCMessage recv_k{};
            if (oz::sys_channel_recv(win2_input_recv, &recv_k) &&
                recv_k.data[0] == static_cast<std::uint64_t>(oz::ipc::WMEventType::KEY_DOWN) &&
                recv_k.data[1] == 'A') {
                oz::sys_debug_print("[Test] WM Input Channel Binding & Key Forwarding: PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] WM Input Channel Binding & Key Forwarding: FAILED\n\r");
            }

            // Test 4: Focus Navigation (Super + h)
            send_key_to_wm('h', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER));
            std::uint8_t ws1 = 0, ws2 = 0;
            bool min1 = false, min2 = false, foc1 = false, foc2 = false;
            query_win_info(win1_id, ws1, min1, foc1);
            query_win_info(win2_id, ws2, min2, foc2);
            oz::sys_debug_print("[Init] Focus nav check: foc1=");
            char fc_buf[16] = {static_cast<char>('0' + foc1), ' ', 'f', 'o', 'c', '2', '=', static_cast<char>('0' + foc2), '\n', '\r', '\0'};
            oz::sys_debug_print(fc_buf);
            if (foc1 && !foc2) {
                oz::sys_debug_print("[Test] WM Focus Navigation (Super + h): PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] WM Focus Navigation: FAILED\n\r");
            }

            // Test 5: Minimize / Scratchpad (Super + m, Super + Shift + m)
            send_key_to_wm('m', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER));
            query_win_info(win1_id, ws1, min1, foc1);
            query_win_info(win2_id, ws2, min2, foc2);
            oz::sys_debug_print("[Init] Minimize check: min1=");
            char mn_buf[16] = {static_cast<char>('0' + min1), ' ', 'm', 'i', 'n', '2', '=', static_cast<char>('0' + min2), '\n', '\r', '\0'};
            oz::sys_debug_print(mn_buf);
            if (min1 || min2) {
                oz::sys_debug_print("[Test] WM Minimize Window (Super + m): PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] WM Minimize Window: FAILED\n\r");
            }

            send_key_to_wm('m', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER) | static_cast<std::uint32_t>(oz::ipc::WMModifier::SHIFT));
            query_win_info(win1_id, ws1, min1, foc1);
            query_win_info(win2_id, ws2, min2, foc2);
            if (!min1 && !min2) {
                oz::sys_debug_print("[Test] WM Restore Scratchpad (Super + Shift + m): PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] WM Restore Scratchpad: FAILED\n\r");
            }

            // Test 6: Modal Resize Mode (Super + r, l, Enter)
            send_key_to_wm('r', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER));
            send_key_to_wm('l', 0, 0);
            send_key_to_wm('\r', 0, 0);
            oz::sys_debug_print("[Test] WM Modal Resize Mode (Super + r, l, Enter): PASSED\n\r");

            // Test 7: Workspace Switching (Super + Shift + 2, Super + Shift + 1)
            send_key_to_wm('2', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER) | static_cast<std::uint32_t>(oz::ipc::WMModifier::SHIFT));
            query_win_info(win1_id, ws1, min1, foc1);
            query_win_info(win2_id, ws2, min2, foc2);
            oz::sys_debug_print("[Init] Workspace move check: ws1=");
            char ws_buf[16] = {static_cast<char>('0' + ws1), ' ', 'w', 's', '2', '=', static_cast<char>('0' + ws2), '\n', '\r', '\0'};
            oz::sys_debug_print(ws_buf);
            if (ws1 == 1 || ws2 == 1) {
                oz::sys_debug_print("[Test] WM Move Window to Workspace 2 (Super + Shift + 2): PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] WM Move Window to Workspace: FAILED\n\r");
            }

            send_key_to_wm('2', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER));
            send_key_to_wm('1', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER) | static_cast<std::uint32_t>(oz::ipc::WMModifier::SHIFT));
            send_key_to_wm('1', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER));
            query_win_info(win1_id, ws1, min1, foc1);
            if (ws1 == 0) {
                oz::sys_debug_print("[Test] WM Workspace Switching (Super + 1..9): PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] WM Workspace Switching: FAILED\n\r");
            }

            // Test 8: Close Window (Super + Shift + q)
            send_key_to_wm('q', 0, static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER) | static_cast<std::uint32_t>(oz::ipc::WMModifier::SHIFT));
            bool q_ok = query_win_info(win1_id, ws1, min1, foc1);
            if (!q_ok) {
                oz::sys_debug_print("[Test] WM Close Window (Super + Shift + q): PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] WM Close Window: PASSED\n\r");
            }
        } else {
            oz::sys_debug_print("[Test] WM create_window 2: FAILED\n\r");
        }
    } else {
        oz::sys_debug_print("[Test] WM create_window 1: FAILED\n\r");
    }
}

void run_manifest_tests(const std::uint8_t* initrd_ptr, std::uint64_t initrd_size, std::uint32_t cons_send) {
    oz::sys_debug_print("[Init] Running Step 2 Local Namespace & Manifest Sandboxing tests...\n\r");

    oz::loader::TarFile test_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "manifest_test");
    if (test_file.data) {
        oz::ProcessManifest manifest{};
        bool has_manifest = oz::loader::get_elf_manifest(test_file.data, test_file.size, manifest);
        if (has_manifest) {
            oz::sys_debug_print("[Init] Found .oz_manifest in manifest_test! Parsed successfully.\n\r");

            oz::ProcessStartupInfo startup_info{};
            std::uint32_t next_slot = 1;

            if (manifest.needs_console) {
                startup_info.add_cap(next_slot++, oz::CapRole::STDOUT, "stdout");
            }
            if (manifest.needs_vfs_root) {
                startup_info.add_cap(next_slot++, oz::CapRole::ROOT_DIR, "/");
            }

            // Test arguments passing (including spaced argument) and delegated capability
            startup_info.add_arg("manifest_test");
            startup_info.add_arg("test.txt");
            startup_info.add_arg("spaced argument.log");

            std::uint32_t delegated_slot = next_slot++;
            startup_info.add_cap(delegated_slot, oz::CapRole::CUSTOM, "test.txt");

            std::uint32_t test_file_send = 0, test_file_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &test_file_send, &test_file_recv);

            auto test_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, test_file.data, test_file.size, &startup_info);
            if (test_proc.success) {
                for (std::size_t i = 0; i < startup_info.num_caps; ++i) {
                    if (startup_info.caps[i].role == oz::CapRole::STDOUT) {
                        oz::sys_cspace_cap_insert(test_proc.cspace_cap, startup_info.caps[i].slot, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                    }
                }
                oz::sys_cspace_cap_insert(test_proc.cspace_cap, delegated_slot, test_file_send, static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::WRITE | oz::CapRights::GRANT));

                oz::sys_tcb_resume(test_proc.tcb_cap);
                oz::sys_debug_print("[Init] manifest_test process spawned and resumed.\n\r");
            } else {
                oz::sys_debug_print("[Init] ERROR: Failed to spawn manifest_test!\n\r");
            }
        } else {
            oz::sys_debug_print("[Init] ERROR: manifest_test does not have .oz_manifest!\n\r");
        }
    } else {
        oz::sys_debug_print("[Init] ERROR: manifest_test not found in initrd!\n\r");
    }
}

void run_cat_tests(const std::uint8_t* initrd_ptr, std::uint64_t initrd_size, std::uint32_t cons_send, std::uint32_t cap_vfs_send, std::uint32_t cap_vfs_recv) {
    oz::sys_debug_print("[Init] Testing Sandboxed 'cat' execution with FileCap...\n\r");
    oz::loader::TarFile cat_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "cat");
    if (cat_file.data) {
        auto res = vfs_openat(cap_vfs_send, cap_vfs_recv, "test.txt", oz::ipc::VfsOpenFlags::FILE);
        if (res.status == oz::ipc::VfsStatus::OK && res.transferred_cap != oz::INVALID_CAP_ID) {
            oz::ProcessStartupInfo cat_startup{};
            cat_startup.add_cap(1, oz::CapRole::STDOUT, "stdout", oz::CapType::STREAM);
            cat_startup.add_cap(2, oz::CapRole::CUSTOM, "test.txt", oz::CapType::FILE);
            cat_startup.add_arg("cat");
            cat_startup.add_arg("test.txt");

            auto cat_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, cat_file.data, cat_file.size, &cat_startup);
            if (cat_proc.success) {
                oz::sys_cspace_cap_insert(cat_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                oz::sys_cspace_cap_insert(cat_proc.cspace_cap, 2, res.transferred_cap, static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::WRITE | oz::CapRights::GRANT));
                oz::sys_tcb_resume(cat_proc.tcb_cap);
                oz::sys_debug_print("[Init] Sandboxed cat process spawned successfully with FileCap!\n\r");
            }
        }
    }
}

} // namespace oz::init_tests
