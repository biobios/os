#include "syscall.hpp"
#include "loader.hpp"
#include "../common/test_harness.hpp"
#include "../../src/common/ipc/Protocol.hpp"
#include "../../src/common/utils/BootInfo.hpp"

namespace oz::vtest {

static constexpr std::uint64_t BOOTINFO_VADDR = 0x600000000000ULL;
static constexpr std::uint64_t INITRD_VADDR   = 0x500000000000ULL;
static constexpr std::uint32_t CAP_AHCI_SEND   = 2;
static constexpr std::uint32_t CAP_VFS_SEND    = 3;
static constexpr std::uint32_t CAP_VFS_RECV    = 4;

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

static void run_tests() {
    oz::sys_debug_print("\n\r--- VFS & Storage Integration Tests ---\n\r");

    // Test 1: Valid relative open of file
    auto res_file = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "test.txt", oz::ipc::VfsOpenFlags::FILE);
    TEST_REPORT("VTEST", "VFS openat relative file (Cap Transfer)", res_file.status == oz::ipc::VfsStatus::OK && res_file.transferred_cap != oz::INVALID_CAP_ID);

    // Test 2: Reject ".." in relative path (Sandbox violation)
    auto res_jailbreak = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "../test.txt", oz::ipc::VfsOpenFlags::FILE);
    TEST_REPORT("VTEST", "VFS sandbox '..' traversal rejection", res_jailbreak.status == oz::ipc::VfsStatus::INVALID_PATH);

    // Test 3: Reject leading '/' (Absolute path ambient authority violation)
    auto res_abs = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "/test.txt", oz::ipc::VfsOpenFlags::FILE);
    TEST_REPORT("VTEST", "VFS sandbox absolute path rejection", res_abs.status == oz::ipc::VfsStatus::INVALID_PATH);

    // Test 4: Open subdirectory as Directory Capability
    auto res_dir = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "os", oz::ipc::VfsOpenFlags::DIRECTORY);
    if (res_dir.status == oz::ipc::VfsStatus::OK && res_dir.transferred_cap != oz::INVALID_CAP_ID) {
        TEST_REPORT("VTEST", "VFS openat Directory Capability", true);

        // Test 5: Open child file relative to the new Directory Capability
        auto res_subfile = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "kernel.bin", oz::ipc::VfsOpenFlags::FILE, res_dir.handle);
        TEST_REPORT("VTEST", "VFS openat child relative to DirectoryCap", res_subfile.status == oz::ipc::VfsStatus::OK && res_subfile.transferred_cap != oz::INVALID_CAP_ID);

        // Test 6: Attempt '..' escape from Directory Capability
        auto res_sub_escape = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "../test.txt", oz::ipc::VfsOpenFlags::FILE, res_dir.handle);
        TEST_REPORT("VTEST", "VFS child '..' escape rejection", res_sub_escape.status == oz::ipc::VfsStatus::INVALID_PATH);
    } else {
        TEST_REPORT("VTEST", "VFS openat Directory Capability", false);
    }
}

} // namespace oz::vtest

extern "C" void _start() {
    oz::sys_debug_print("\n\r========================================================\n\r");
    oz::sys_debug_print(" [OZ_OS] Storage & VFS Test Runner (vtest)\n\r");
    oz::sys_debug_print("========================================================\n\r");

    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_BOOTINFO, oz::vtest::BOOTINFO_VADDR, 0x00, 4096) != 0) {
        oz::test::report("VTEST", "Map BootInfo", false);
        oz::test::exit_qemu();
    }
    const volatile auto* boot_info = reinterpret_cast<const volatile oz::BootInfo*>(oz::vtest::BOOTINFO_VADDR);
    std::uint64_t initrd_size = boot_info->initrd_size;
    std::uint64_t ahci_abar_phys = boot_info->ahci_abar_phys;

    if (initrd_size > 0) {
        oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_INITRD, oz::vtest::INITRD_VADDR, 0x00, initrd_size);
    }
    const std::uint8_t* initrd_ptr = reinterpret_cast<const std::uint8_t*>(oz::vtest::INITRD_VADDR + boot_info->initrd_offset);

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

    // 1. Spawn AHCI Driver
    oz::loader::TarFile ahci_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "ahci");
    if (!ahci_file.data) {
        oz::test::report("VTEST", "Find ahci in initrd", false);
        oz::test::exit_qemu();
    }
    auto ahci_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, ahci_file.data, ahci_file.size);
    if (!ahci_proc.success) {
        oz::test::report("VTEST", "Spawn ahci", false);
        oz::test::exit_qemu();
    }

    std::uint32_t init_ahci_send = 0, ahci_init_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &init_ahci_send, &ahci_init_recv);
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, oz::vtest::CAP_AHCI_SEND, init_ahci_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));

    std::uint32_t vfs_ahci_send = 0, ahci_vfs_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_ahci_send, &ahci_vfs_recv);

    std::uint32_t ahci_vfs_send = 0, vfs_ahci_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &ahci_vfs_send, &vfs_ahci_recv);

    std::uint32_t shared_buf_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);
    std::uint32_t notif_ahci = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t notif_vfs = oz::sys_notification_create(oz::CAP_UNTYPED);

    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 2, ahci_init_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 3, ahci_vfs_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 4, ahci_vfs_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 5, notif_ahci, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 6, notif_vfs, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    if (ahci_abar_phys != 0) {
        oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 241, oz::CAP_AHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL));
    }
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 242, shared_buf_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));

    oz::sys_tcb_resume(ahci_proc.tcb_cap);
    init_coroutine(ahci_proc.tcb_cap);

    // Send AHCI MMIO address
    oz::IPCMessage ahci_mmio_msg{};
    ahci_mmio_msg.data[0] = ahci_abar_phys;
    oz::IPCMessage ahci_resp{};
    oz::sys_call(oz::vtest::CAP_AHCI_SEND, &ahci_mmio_msg, &ahci_resp);

    // 2. Spawn VFS Server
    oz::loader::TarFile vfs_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "vfs");
    if (!vfs_file.data) {
        oz::test::report("VTEST", "Find vfs in initrd", false);
        oz::test::exit_qemu();
    }
    auto vfs_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, vfs_file.data, vfs_file.size);
    if (!vfs_proc.success) {
        oz::test::report("VTEST", "Spawn vfs", false);
        oz::test::exit_qemu();
    }

    std::uint32_t vfs_req_send = 0, vfs_req_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_req_send, &vfs_req_recv);

    std::uint32_t vfs_init_reply_send = 0, init_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_init_reply_send, &init_reply_recv);

    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, oz::vtest::CAP_VFS_SEND, vfs_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, oz::vtest::CAP_VFS_RECV, init_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));

    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 2, vfs_ahci_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 3, vfs_ahci_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 4, vfs_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 5, vfs_init_reply_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 8, notif_ahci, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 9, notif_vfs, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 242, shared_buf_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));

    std::uint32_t init_client_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 243, init_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));

    oz::sys_tcb_resume(vfs_proc.tcb_cap);
    init_coroutine(vfs_proc.tcb_cap);

    // Run VFS tests
    oz::vtest::run_tests();

    oz::test::print_summary("VTEST");
    oz::test::exit_qemu();
}
