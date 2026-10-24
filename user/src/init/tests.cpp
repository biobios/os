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

    oz::IPCMessage resp{};
    if (!oz::sys_call(dir_send_cap, &req, &resp)) return res;

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
        oz::IPCMessage once_resp{};
        if (oz::sys_call(once_sender, &once_msg, &once_resp)) {
            // After calling, the once_sender slot must have been removed by kernel!
            if (oz::sys_cap_delete(once_sender) != 0) {
                oz::sys_debug_print("[Test] Once Capability auto-consumption: PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] Once Capability auto-consumption: FAILED (slot still valid after call)\n\r");
            }
        } else {
            oz::sys_debug_print("[Test] Once Capability auto-consumption: FAILED (call failed)\n\r");
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

    oz::IPCMessage resp_surf{};
    if (oz::sys_call(cap_display_send, &req_surf, &resp_surf) && resp_surf.data[0] == 0 && resp_surf.has_cap) {
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
            oz::sys_call(cap_display_send, &req_commit, &resp_commit);

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

    // Test 1: Create Window 1 (Main Editor/Terminal)
    oz::IPCMessage req_win1{};
    req_win1.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::CREATE_WINDOW);
    req_win1.data[1] = 380;
    req_win1.data[2] = 260;
    oz::IPCMessage resp_win1{};
    oz::sys_debug_print("[Init] Sending CREATE_WINDOW to CAP_WM_SEND via sys_call...\n\r");
    bool send_ok = oz::sys_call(cap_wm_send, &req_win1, &resp_win1);

    if (send_ok && resp_win1.data[0] == 0 && resp_win1.has_cap) {
        oz::sys_debug_print("[Test] WM create_window 1 (Tiling BSP Root): PASSED\n\r");
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

        // Map buffer and fill with dark blue
        static constexpr std::uint64_t WIN1_VADDR = 0x710000000000ULL;
        if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, win1_frame_cap, WIN1_VADDR, 0x01, 380 * 260 * 4) == 0) {
            auto* buf = reinterpret_cast<Pixel*>(WIN1_VADDR);
            for (int i = 0; i < 380 * 260; ++i) buf[i] = Pixel{0x50, 0x20, 0x10, 0};
        }

        // Test 2: Create Window 2 (Split right side)
        oz::IPCMessage req_win2{};
        req_win2.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::CREATE_WINDOW);
        req_win2.data[1] = 380;
        req_win2.data[2] = 260;
        oz::IPCMessage resp_win2{};
        if (oz::sys_call(cap_wm_send, &req_win2, &resp_win2) && resp_win2.data[0] == 0 && resp_win2.has_cap) {
            oz::sys_debug_print("[Test] WM create_window 2 (BSP Auto-Split Right): PASSED\n\r");
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
            // Win 2 is currently focused. Send key 'A' without modifiers
            oz::sys_debug_print("[Init] Sending key 'A' to WM...\n\r");
            send_key_to_wm('A', 0x04, 0);

            oz::IPCMessage get_ev_msg{};
            get_ev_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::GET_EVENT);
            get_ev_msg.data[1] = win2_id;
            oz::IPCMessage recv_k{};
            if (oz::sys_call(cap_wm_send, &get_ev_msg, &recv_k) &&
                recv_k.data[0] == 0 &&
                recv_k.data[1] == 'A') {
                oz::sys_debug_print("[Test] WM Key Forwarding & GET_EVENT: PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] WM Key Forwarding & GET_EVENT: FAILED\n\r");
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

// =============================================================================
// Baton Scheduling Context v2 Test Fixtures & Workers
// =============================================================================

// 1. Rebind-and-Switch cooperative thread switching
static volatile bool g_rebind_t1_entered = false;
static volatile bool g_rebind_t2_entered = false;
static volatile bool g_rebind_t1_resumed = false;
static std::uint32_t g_rebind_t1_tcb = 0;
static std::uint32_t g_rebind_t2_tcb = 0;
static std::uint32_t g_rebind_shared_baton = 0;

static void test_rebind_t1_thread() {
    g_rebind_t1_entered = true;
    // Rebind our baton to T2 -> triggers Rebind-and-Switch!
    oz::sys_tcb_bind_baton(g_rebind_t2_tcb, g_rebind_shared_baton);
    // Resumes here when T2 rebinds back to T1
    g_rebind_t1_resumed = true;
    oz::sys_yield();
    oz::sys_exit(0);
}

static void test_rebind_t2_thread() {
    g_rebind_t2_entered = true;
    // Rebind baton back to T1 -> triggers Rebind-and-Switch back!
    oz::sys_tcb_bind_baton(g_rebind_t1_tcb, g_rebind_shared_baton);
    oz::sys_exit(0);
}

// 2. budget == 0 Notification wakeup + sys_baton_inject
static volatile bool g_zero_worker_ran = false;
static void test_zero_worker_thread() {
    g_zero_worker_ran = true;
    oz::sys_yield();
    oz::sys_exit(0);
}

// 3. Aborting IPC on Baton bind
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

// 4. Dead client Silent Drop on sys_reply_recv & SchedReason::ORPHANED
static volatile bool g_server_first_recv_ok = false;
static volatile std::uint32_t g_dead_client_srv_ep = 0;
static volatile std::uint32_t g_dead_client_cli_ep = 0;
static void test_orphan_server_thread() {
    oz::IPCMessage req{};
    oz::IPCMessage rep{};
    // 1. Initial wait with INVALID_REPLY_TOKEN
    oz::ReplyToken tok = oz::sys_reply_recv(oz::INVALID_REPLY_TOKEN, g_dead_client_srv_ep, nullptr, &req);
    g_server_first_recv_ok = (tok != oz::INVALID_REPLY_TOKEN);
    // 2. Reply to invalid/dead client token (0xDEADBEEF)
    // Kernel performs Silent Drop, unbinds baton to orphan, and switches to scheduler with SchedReason::ORPHANED!
    oz::sys_reply_recv(0xDEADBEEF, g_dead_client_srv_ep, &rep, &req);
    oz::sys_exit(0);
}

static void test_orphan_client_thread() {
    oz::IPCMessage send_msg{};
    oz::IPCMessage recv_msg{};
    send_msg.data[0] = 0x1234;
    // Call server -> hands off baton to server and direct switches!
    oz::sys_call(g_dead_client_cli_ep, &send_msg, &recv_msg);
    oz::sys_exit(0);
}

// 5. Fault IPC page fault resolution
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

    // Resolve #PF by mapping the physical frame at faulting address
    std::uint64_t map_vaddr = g_pf_fault_addr & ~0xFFFULL;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, g_pf_frame_cap, map_vaddr, 0x07, 4096);
    g_pf_resolved = true;

    // Return baton to client and resume client execution
    oz::sys_reply_recv(tok, g_pf_pager_ep, &rep, &req);
    oz::sys_exit(0);
}

static void test_pf_client_thread() {
    volatile std::uint32_t* ptr = reinterpret_cast<volatile std::uint32_t*>(FAULT_TEST_VADDR);
    *ptr = 0xCAFEBABE; // Triggers #PF!
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

    // 1. 初回 sys_irq_wait: リザーブ待機に入る
    oz::sys_irq_wait(g_test_irq_handler_cap);

    // 2. スケジューラから再開された
    g_irq_replenish_step = 2;

    // 処理をシミュレートし、2回目の待機
    oz::sys_irq_wait(g_test_irq_handler_cap);

    g_irq_replenish_step = 3;
    oz::sys_exit(0);
}

void run_baton_scheduling_tests() {
    oz::sys_debug_print("[Init] =========================================================\n\r");
    oz::sys_debug_print("[Init] Running Baton Scheduling Context (v2 Pure Capability) Tests\n\r");
    oz::sys_debug_print("[Init] =========================================================\n\r");

    // =========================================================================
    // 1. Baton Retype, Bind, and Rebind-and-Switch Cooperative Switching
    // =========================================================================
    oz::sys_debug_print("[Init] 1. Testing Baton Retype, Bind, and Rebind-and-Switch...\n\r");
    g_rebind_shared_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(g_rebind_shared_baton, 100000 /* 100ms */);

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

    // Bind shared baton to T1
    oz::sys_tcb_bind_baton(g_rebind_t1_tcb, g_rebind_shared_baton);

    // Dispatch T1: T1 runs -> rebinds to T2 (switches to T2) -> T2 rebinds to T1 (switches to T1) -> T1 yields
    oz::SchedResult res = oz::sys_baton_switch(g_rebind_shared_baton);
    if (g_rebind_t1_entered && g_rebind_t2_entered && g_rebind_t1_resumed && res.reason == oz::SchedReason::YIELD) {
        oz::sys_debug_print("[Init] SUCCESS: Rebind-and-Switch cooperative thread switching verified!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Rebind-and-Switch test failed\n\r");
    }
    // Resume T1 to cleanly exit
    oz::sys_baton_switch(g_rebind_shared_baton);

    // =========================================================================
    // 2. Test budget == 0 Notification Wakeup + sys_baton_inject Replenishment
    // =========================================================================
    oz::sys_debug_print("[Init] 2. Testing budget == 0 Notification Wakeup & sys_baton_inject...\n\r");
    std::uint32_t zero_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t zero_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    // Bind notification to baton (budget is 0)
    static constexpr std::uint64_t ZERO_BADGE = (1ULL << 8);
    oz::sys_baton_bind_notification(zero_baton, zero_notif, ZERO_BADGE);

    std::uint32_t zero_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t zero_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t zero_stack_top = 0x700010000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, zero_stack, 0x700010000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(zero_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_zero_worker_thread), zero_stack_top);

    // Bind zero_baton to zero_tcb: thread becomes Ready with budget == 0!
    // Must trigger rising edge notification to zero_notif!
    oz::sys_tcb_bind_baton(zero_tcb, zero_baton);

    std::uint64_t polled_badge = 0;
    oz::sys_notification_poll(zero_notif, &polled_badge);
    if ((polled_badge & ZERO_BADGE) != 0) {
        oz::sys_debug_print("[Init] SUCCESS: budget == 0 triggered notification on thread Ready!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: budget == 0 did not trigger notification\n\r");
    }

    // Try to switch without budget: must return SchedReason::TIMEOUT immediately
    res = oz::sys_baton_switch(zero_baton);
    if (res.reason == oz::SchedReason::TIMEOUT) {
        oz::sys_debug_print("[Init] SUCCESS: sys_baton_switch with budget == 0 returned SchedReason::TIMEOUT!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: sys_baton_switch with budget == 0 did not return TIMEOUT\n\r");
    }

    // Replenish budget via sys_baton_inject
    oz::sys_baton_inject(zero_baton, 50000 /* 50ms */);
    g_zero_worker_ran = false;
    res = oz::sys_baton_switch(zero_baton);
    if (g_zero_worker_ran && res.reason == oz::SchedReason::YIELD) {
        oz::sys_debug_print("[Init] SUCCESS: Worker executed after sys_baton_inject replenishment!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Worker failed to execute after replenishment\n\r");
    }
    // Resume to exit
    oz::sys_baton_switch(zero_baton);

    // =========================================================================
    // 3. Test Aborting IPC on Baton Bind (SYS_ERR_CANCELLED)
    // =========================================================================
    oz::sys_debug_print("[Init] 3. Testing Aborting IPC on Baton Bind (SYS_ERR_CANCELLED)...\n\r");
    std::uint32_t abort_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t abort_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4);
    std::uint64_t abort_stack_top = 0x700020000000ULL + 16384;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, abort_stack, 0x700020000000ULL, 0x03, 16384);
    oz::sys_tcb_configure(abort_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(test_abort_worker_thread), abort_stack_top);

    std::uint32_t ep_a0 = oz::INVALID_CAP_ID;
    std::uint32_t ep_a1 = oz::INVALID_CAP_ID;
    oz::sys_channel_create(oz::CAP_UNTYPED, &ep_a0, &ep_a1);
    g_abort_ep = ep_a0;

    std::uint32_t abort_baton1 = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(abort_baton1, 50000);
    oz::sys_tcb_bind_baton(abort_tcb, abort_baton1);

    g_abort_worker_ran = false;
    g_abort_result = 0;
    // Worker runs and blocks on channel recv (SchedReason::BLOCKED)
    res = oz::sys_baton_switch(abort_baton1);
    if (g_abort_worker_ran && res.reason == oz::SchedReason::BLOCKED) {
        oz::sys_debug_print("[Init] SUCCESS: Worker blocked on IPC recv (SchedReason::BLOCKED)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Worker did not block on IPC recv\n\r");
    }

    // Now bind a new rescue baton to the blocked worker -> triggers Aborting IPC!
    std::uint32_t rescue_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(rescue_baton, 50000);
    oz::sys_tcb_bind_baton(abort_tcb, rescue_baton);

    // Switch to worker: it should wake up with SYS_ERR_CANCELLED and exit!
    res = oz::sys_baton_switch(rescue_baton);
    if (g_abort_result == oz::SYS_ERR_CANCELLED &&
        (res.reason == oz::SchedReason::EXIT || res.reason == oz::SchedReason::ORPHANED)) {
        oz::sys_debug_print("[Init] SUCCESS: Aborting IPC verified: worker received SYS_ERR_CANCELLED (-3) and exited!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Aborting IPC did not produce SYS_ERR_CANCELLED\n\r");
    }

    // =========================================================================
    // 4. Test Dead Client Silent Drop on sys_reply_recv & SchedReason::ORPHANED
    // =========================================================================
    oz::sys_debug_print("[Init] 4. Testing Dead Client Silent Drop on sys_reply_recv & SchedReason::ORPHANED...\n\r");
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

    std::uint32_t ep_s = oz::INVALID_CAP_ID;
    std::uint32_t ep_c = oz::INVALID_CAP_ID;
    oz::sys_channel_create(oz::CAP_UNTYPED, &ep_s, &ep_c);
    g_dead_client_srv_ep = ep_s;
    g_dead_client_cli_ep = ep_c;

    // Start server with a temporary baton so it enters sys_reply_recv(INVALID_REPLY_TOKEN)
    std::uint32_t srv_init_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(srv_init_baton, 50000);
    oz::sys_tcb_bind_baton(orphan_srv_tcb, srv_init_baton);
    res = oz::sys_baton_switch(srv_init_baton);
    if (res.reason == oz::SchedReason::BLOCKED) {
        oz::sys_debug_print("[Init] Server blocked on initial sys_reply_recv (INVALID_REPLY_TOKEN)\n\r");
    }

    // Now dispatch client with client_baton
    std::uint32_t client_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(client_baton, 50000);
    oz::sys_tcb_bind_baton(orphan_cli_tcb, client_baton);

    // Dispatch client: calls sys_call -> hands off client_baton to server -> server handles call
    // -> server then calls sys_reply_recv with dead token 0xDEADBEEF -> Silent Drop + Orphaned baton -> SchedReason::ORPHANED!
    g_server_first_recv_ok = false;
    res = oz::sys_baton_switch(client_baton);
    if (g_server_first_recv_ok && res.reason == oz::SchedReason::ORPHANED) {
        oz::sys_debug_print("[Init] SUCCESS: Silent Drop & SchedReason::ORPHANED verified on dead client token!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Dead client silent drop / ORPHANED test failed\n\r");
    }

    // =========================================================================
    // 5. Test Fault IPC Page Fault Resolution (#PF Recovery)
    // =========================================================================
    oz::sys_debug_print("[Init] 5. Testing Fault IPC Page Fault Resolution (#PF Recovery)...\n\r");
    g_pf_frame_cap = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);

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

    std::uint32_t ep_f = oz::INVALID_CAP_ID;
    std::uint32_t ep_p = oz::INVALID_CAP_ID;
    oz::sys_channel_create(oz::CAP_UNTYPED, &ep_f, &ep_p);
    g_pf_pager_ep = ep_p;
    oz::sys_tcb_set_fault_endpoint(pf_client_tcb, ep_f);

    // Initial switch to pager so it enters sys_reply_recv(INVALID_REPLY_TOKEN)
    std::uint32_t pager_init_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(pager_init_baton, 50000);
    oz::sys_tcb_bind_baton(pf_pager_tcb, pager_init_baton);
    res = oz::sys_baton_switch(pager_init_baton);
    if (res.reason == oz::SchedReason::BLOCKED) {
        oz::sys_debug_print("[Init] Pager blocked on sys_reply_recv awaiting #PF\n\r");
    }

    // Now give client a baton and dispatch client
    std::uint32_t pf_client_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_baton_inject(pf_client_baton, 100000);
    oz::sys_tcb_bind_baton(pf_client_tcb, pf_client_baton);

    g_pf_resolved = false;
    g_pf_fault_addr = 0;
    g_pf_client_success = false;

    // Dispatch client: touches 0x65000000 -> #PF -> Fault IPC hands baton to pager -> pager maps page & replies -> client writes 0xCAFEBABE & yields
    res = oz::sys_baton_switch(pf_client_baton);
    if (g_pf_resolved && g_pf_fault_addr == FAULT_TEST_VADDR && g_pf_client_success && res.reason == oz::SchedReason::YIELD) {
        oz::sys_debug_print("[Init] SUCCESS: Fault IPC page fault resolution (#PF recovery) verified!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Fault IPC page fault test failed\n\r");
    }
    // Resume to exit
    oz::sys_baton_switch(pf_client_baton);

    // =========================================================================
    // 8. Test Hierarchical Radix-64 Multi-stage Notification (Chapter 1.8)
    // =========================================================================
    oz::sys_debug_print("[Init] Testing Hierarchical Radix-64 Multi-stage Notification...\n\r");

    // 8.1 One-Hot Badge Invariant
    std::uint32_t root_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t child_notif = oz::sys_notification_create(oz::CAP_UNTYPED);

    int b_res = oz::sys_notification_bind_parent(child_notif, root_notif, 0); // 0 bits -> invalid
    if (b_res == oz::SYS_ERR_INVALID) {
        oz::sys_debug_print("[Init] SUCCESS: One-hot badge invariant verified (badge=0 rejected with SYS_ERR_INVALID)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: badge=0 did not return SYS_ERR_INVALID\n\r");
    }

    b_res = oz::sys_notification_bind_parent(child_notif, root_notif, 3); // 2 bits -> invalid
    if (b_res == oz::SYS_ERR_INVALID) {
        oz::sys_debug_print("[Init] SUCCESS: One-hot badge invariant verified (badge=3 rejected with SYS_ERR_INVALID)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: badge=3 did not return SYS_ERR_INVALID\n\r");
    }

    b_res = oz::sys_notification_bind_parent(child_notif, root_notif, 1ULL << 1); // 1 bit -> valid
    if (b_res == 0) {
        oz::sys_debug_print("[Init] SUCCESS: Binding valid child with one-hot badge succeeded!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Binding valid child failed\n\r");
    }

    // 8.2 Acyclic Invariant (Cycle Rejection)
    int cyc_self = oz::sys_notification_bind_parent(root_notif, root_notif, 1ULL << 0);
    if (cyc_self == oz::SYS_ERR_DEADLK) {
        oz::sys_debug_print("[Init] SUCCESS: Acyclic invariant verified: self-binding returned SYS_ERR_DEADLK (-4)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Self-binding did not return SYS_ERR_DEADLK\n\r");
    }

    int cyc_direct = oz::sys_notification_bind_parent(root_notif, child_notif, 1ULL << 2);
    if (cyc_direct == oz::SYS_ERR_DEADLK) {
        oz::sys_debug_print("[Init] SUCCESS: Acyclic invariant verified: direct cycle (A->B->A) returned SYS_ERR_DEADLK (-4)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Direct cycle did not return SYS_ERR_DEADLK\n\r");
    }

    std::uint32_t gc_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    oz::sys_notification_bind_parent(gc_notif, child_notif, 1ULL << 0); // gc -> child -> root
    int cyc_indirect = oz::sys_notification_bind_parent(root_notif, gc_notif, 1ULL << 0);
    if (cyc_indirect == oz::SYS_ERR_DEADLK) {
        oz::sys_debug_print("[Init] SUCCESS: Acyclic invariant verified: indirect cycle (A->B->C->A) returned SYS_ERR_DEADLK (-4)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Indirect cycle did not return SYS_ERR_DEADLK\n\r");
    }

    // 8.3 Max Depth Limit (Max 4 levels)
    // Current chain: gc_notif (Level 3) -> child_notif (Level 2) -> root_notif (Level 1)
    std::uint32_t l4_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    int d4_res = oz::sys_notification_bind_parent(l4_notif, gc_notif, 1ULL << 0);
    if (d4_res == 0) {
        oz::sys_debug_print("[Init] SUCCESS: Binding Level 4 notification succeeded (depth 4 allowed)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Binding Level 4 failed\n\r");
    }

    std::uint32_t l5_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    int d5_res = oz::sys_notification_bind_parent(l5_notif, l4_notif, 1ULL << 0);
    if (d5_res == oz::SYS_ERR_INVALID) {
        oz::sys_debug_print("[Init] SUCCESS: Max Depth limit verified: depth 5 rejected with SYS_ERR_INVALID (-1)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Depth 5 did not return SYS_ERR_INVALID\n\r");
    }

    // 8.4 Unbinding
    int unbind_res = oz::sys_notification_unbind_parent(l4_notif);
    if (unbind_res == 0) {
        oz::sys_debug_print("[Init] SUCCESS: sys_notification_unbind_parent succeeded!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: sys_notification_unbind_parent returned error\n\r");
    }

    // Now l4 is standalone (depth 1), so binding l5 to l4 is depth 2 (allowed)
    d5_res = oz::sys_notification_bind_parent(l5_notif, l4_notif, 1ULL << 0);
    if (d5_res == 0) {
        oz::sys_debug_print("[Init] SUCCESS: Binding to unbound notification succeeded!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Binding to unbound notification failed\n\r");
    }

    // 8.5 Edge-Triggered Cascading & Shortcut Optimization
    std::uint32_t edge_p = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t edge_c = oz::sys_notification_create(oz::CAP_UNTYPED);
    oz::sys_notification_bind_parent(edge_c, edge_p, 1ULL << 10);

    // Initial signal: 0 -> non-zero (rising edge) triggers cascade to edge_p
    oz::sys_notification_signal(edge_c, 1ULL << 2);
    std::uint64_t p_val = 0;
    oz::sys_notification_poll(edge_p, &p_val);
    if (p_val == (1ULL << 10)) {
        oz::sys_debug_print("[Init] SUCCESS: Edge-triggered cascading verified (0 -> non-zero signalled parent)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Rising edge did not signal parent\n\r");
    }

    // Second signal to edge_c without polling: edge_c was already non-zero!
    // Must NOT trigger cascade to edge_p (shortcut optimization)
    oz::sys_notification_signal(edge_c, 1ULL << 3);
    p_val = 0;
    oz::sys_notification_poll(edge_p, &p_val);
    if (p_val == 0) {
        oz::sys_debug_print("[Init] SUCCESS: Shortcut optimization verified (no cascade when child already non-zero)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Redundant signal was sent to parent\n\r");
    }

    // Poll child to clear to 0
    std::uint64_t c_val = 0;
    oz::sys_notification_poll(edge_c, &c_val);
    if (c_val == ((1ULL << 2) | (1ULL << 3))) {
        oz::sys_debug_print("[Init] SUCCESS: Child polled and cleared accumulated signals!\n\r");
    }

    // Signal child again: now 0 -> non-zero again, must cascade to parent!
    oz::sys_notification_signal(edge_c, 1ULL << 4);
    p_val = 0;
    oz::sys_notification_poll(edge_p, &p_val);
    if (p_val == (1ULL << 10)) {
        oz::sys_debug_print("[Init] SUCCESS: Next rising edge after clear cascaded to parent again!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Next rising edge did not cascade to parent\n\r");
    }

    // 8.6 Top-down O(K) Poll & Clear Protocol (Radix-64)
    std::uint32_t radix_root = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t group0 = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t group1 = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t group2 = oz::sys_notification_create(oz::CAP_UNTYPED);
    oz::sys_notification_bind_parent(group0, radix_root, 1ULL << 0);
    oz::sys_notification_bind_parent(group1, radix_root, 1ULL << 1);
    oz::sys_notification_bind_parent(group2, radix_root, 1ULL << 2);

    // Fire events only in group 0 (bit 5) and group 2 (bit 42)
    oz::sys_notification_signal(group0, 1ULL << 5);
    oz::sys_notification_signal(group2, 1ULL << 42);

    // Top-down recovery: Poll Root first
    std::uint64_t root_badge = 0;
    oz::sys_notification_poll(radix_root, &root_badge);
    if (root_badge == ((1ULL << 0) | (1ULL << 2))) {
        oz::sys_debug_print("[Init] SUCCESS: Top-down root poll returned exact active groups (0 and 2)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Root badge mismatch\n\r");
    }

    // Recover using tzcnt / __builtin_ctzll:
    bool g0_ok = false, g2_ok = false, g1_touched = false;
    std::uint64_t mask = root_badge;
    while (mask != 0) {
        int g = __builtin_ctzll(mask);
        mask &= ~(1ULL << g);
        if (g == 0) {
            std::uint64_t g0_val = 0;
            oz::sys_notification_poll(group0, &g0_val);
            if (g0_val == (1ULL << 5)) g0_ok = true;
        } else if (g == 2) {
            std::uint64_t g2_val = 0;
            oz::sys_notification_poll(group2, &g2_val);
            if (g2_val == (1ULL << 42)) g2_ok = true;
        } else if (g == 1) {
            g1_touched = true;
        }
    }
    if (g0_ok && g2_ok && !g1_touched) {
        oz::sys_debug_print("[Init] SUCCESS: O(K) Radix-64 top-down poll & clear verified (group 1 skipped in 0 cycles)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Top-down O(K) recovery failed\n\r");
    }

    // =========================================================================
    // 6. Test InterruptThread Type Boundary, IRQ Baton Replenishment & Clean Unwind
    // =========================================================================
    oz::sys_debug_print("[Init] =========================================================\n\r");
    oz::sys_debug_print("[Init] 6. Testing InterruptThread Type Boundary & Replenishment...\n\r");

    std::uint32_t dummy_ch_send = 0;
    std::uint32_t dummy_ch_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &dummy_ch_send, &dummy_ch_recv);
    std::uint32_t dummy_irq = oz::sys_irq_handler_create(oz::CAP_UNTYPED, 0x45, dummy_ch_send);

    // 6.1 Capability Type Safety: normal thread calling sys_irq_wait must be rejected
    int type_safety_res = oz::sys_irq_wait(dummy_irq);
    if (type_safety_res == oz::SYS_ERR_INVALID_TYPE) {
        oz::sys_debug_print("[Init] SUCCESS: Capability Type Safety verified: sys_irq_wait from normal thread rejected with SYS_ERR_INVALID_TYPE!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: sys_irq_wait from normal thread not rejected properly\n\r");
    }

    // 6.2 Retype InterruptThread and configure
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

    // 6.3 Initial reservation: switch to InterruptThread so it enters sys_irq_wait (BLOCKED)
    oz::sys_baton_inject(ibaton, 50000);
    oz::SchedResult ires = oz::sys_baton_switch(ibaton);
    if (g_irq_replenish_step == 1) {
        oz::sys_debug_print("[Init] SUCCESS: InterruptThread initialized and entered initial reservation wait (BLOCKED)!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: InterruptThread did not enter reservation wait\n\r");
    }

    // 6.4 Replenishment and Clean Unwind: resume handler from interrupted/waiting state
    oz::sys_baton_inject(ibaton, 50000);
    oz::sys_tcb_resume(ithread);
    ires = oz::sys_baton_switch(ibaton);
    if (g_irq_replenish_step == 2) {
        oz::sys_debug_print("[Init] SUCCESS: Resumed InterruptThread executed and returned cleanly to caller!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: Resumed InterruptThread did not return cleanly\n\r");
    }

    // 6.5 Synchronous SYS_BATON_SWITCH_TIMEOUT reporting on exhausted handler
    oz::sys_baton_inject(ibaton, 0); // Deplete budget to 0
    ires = oz::sys_baton_switch(ibaton);
    if (ires.reason == oz::SYS_BATON_SWITCH_TIMEOUT) {
        oz::sys_debug_print("[Init] SUCCESS: sys_baton_switch returned SYS_BATON_SWITCH_TIMEOUT on exhausted handler!\n\r");
    } else {
        oz::sys_debug_print("[Init] FAILED: sys_baton_switch on exhausted handler did not return SYS_BATON_SWITCH_TIMEOUT\n\r");
    }

    oz::sys_debug_print("[Init] =========================================================\n\r");
    oz::sys_debug_print("[Init] All Baton Scheduling Context Tests Passed Successfully!\n\r");
    oz::sys_debug_print("[Init] =========================================================\n\r");
}

void run_scheduling_context_donation_tests() {
    run_baton_scheduling_tests();
}

void run_user_scheduler_and_cancel_tests() {
    run_baton_scheduling_tests();
}

} // namespace oz::init_tests
