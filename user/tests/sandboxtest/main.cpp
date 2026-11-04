#include "syscall.hpp"
#include "loader.hpp"
#include "../common/test_harness.hpp"
#include "../../src/common/ipc/Protocol.hpp"
#include "../../src/common/ipc/StartupInfo.hpp"
#include <oz/BootInfo.hpp>

namespace oz::sandboxtest {

static constexpr std::uint64_t BOOTINFO_VADDR = 0x600000000000ULL;
static constexpr std::uint64_t INITRD_VADDR   = 0x500000000000ULL;

static void run_manifest_sandboxing_tests(const std::uint8_t* initrd_ptr, std::uint64_t initrd_size) {
    oz::sys_debug_print("\n\r--- 1. ELF Manifest Parsing & Sandboxing Tests ---\n\r");

    // 1. Locate manifest_test in initrd
    oz::loader::TarFile test_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "manifest_test");
    TEST_REPORT("SANDBOX", "Find manifest_test in initrd", test_file.data != nullptr);
    if (!test_file.data) return;

    // 2. Parse .oz_manifest section
    oz::ProcessManifest manifest{};
    bool has_manifest = oz::loader::get_elf_manifest(test_file.data, test_file.size, manifest);
    TEST_REPORT("SANDBOX", "Parse .oz_manifest section", has_manifest && manifest.magic == oz::ProcessManifest::MAGIC);

    if (has_manifest) {
        bool props_ok = (manifest.requires_service(oz::ServiceId::CONSOLE) &&
                         !manifest.requires_service(oz::ServiceId::VFS_ROOT) &&
                         !manifest.requires_service(oz::ServiceId::DISPLAY) &&
                         !manifest.requires_service(oz::ServiceId::WM) &&
                         manifest.needs_untyped == false);
        TEST_REPORT("SANDBOX", "Manifest static permission declarations match", props_ok);

        // 3. Construct ProcessStartupInfo according to POLA (Principle of Least Authority)
        oz::ProcessStartupInfo startup_info{};
        std::uint32_t next_slot = 1;

        if (manifest.requires_service(oz::ServiceId::CONSOLE)) {
            startup_info.add_cap(next_slot++, oz::CapRole::STDOUT, "stdout");
        }
        if (manifest.requires_service(oz::ServiceId::VFS_ROOT)) {
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

        std::uint32_t dummy_stdout_send = 0, dummy_stdout_recv = 0;
        oz::sys_channel_create(oz::CAP_UNTYPED, &dummy_stdout_send, &dummy_stdout_recv);

        auto test_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, test_file.data, test_file.size, &startup_info);
        TEST_REPORT("SANDBOX", "Spawn sandboxed process with LocalNamespace", test_proc.success);

        if (test_proc.success) {
            for (std::size_t i = 0; i < startup_info.num_caps; ++i) {
                if (startup_info.caps[i].role == oz::CapRole::STDOUT) {
                    oz::sys_cspace_cap_insert(test_proc.cspace_cap, startup_info.caps[i].slot, dummy_stdout_send,
                                              static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                }
            }
            oz::sys_cspace_cap_insert(test_proc.cspace_cap, delegated_slot, test_file_send,
                                      static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::WRITE | oz::CapRights::GRANT));

            // Bind baton and run sandboxed process to completion
            std::uint32_t test_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
            oz::sys_baton_inject(test_baton, 100000);
            oz::sys_tcb_bind_baton(test_proc.tcb_cap, test_baton);
            oz::sys_tcb_resume(test_proc.tcb_cap);

            oz::SchedResult res = oz::sys_baton_switch(test_baton);
            bool clean_exit = (res.reason == oz::SchedReason::EXIT || res.reason == oz::SchedReason::ORPHANED);
            TEST_REPORT("SANDBOX", "manifest_test execution and self-check clean exit (0)", clean_exit);

            oz::sys_cap_delete(test_baton);
        }

        oz::sys_cap_delete(test_file_send);
        oz::sys_cap_delete(test_file_recv);
        oz::sys_cap_delete(dummy_stdout_send);
        oz::sys_cap_delete(dummy_stdout_recv);
    }
}

static void run_cat_sandboxing_tests(const std::uint8_t* initrd_ptr, std::uint64_t initrd_size) {
    oz::sys_debug_print("\n\r--- 2. Sandboxed Command Execution ('cat') Tests ---\n\r");

    oz::loader::TarFile cat_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "cat");
    TEST_REPORT("SANDBOX", "Find cat binary in initrd", cat_file.data != nullptr);
    if (!cat_file.data) return;

    std::uint32_t cons_send = 0, cons_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &cons_send, &cons_recv);

    std::uint32_t dummy_file_send = 0, dummy_file_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &dummy_file_send, &dummy_file_recv);

    oz::ProcessStartupInfo cat_startup{};
    cat_startup.add_cap(1, oz::CapRole::STDOUT, "stdout", oz::StartupNodeType::STREAM);
    cat_startup.add_cap(2, oz::CapRole::CUSTOM, "test.txt", oz::StartupNodeType::FILE);
    cat_startup.add_arg("cat");
    cat_startup.add_arg("test.txt");

    auto cat_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, cat_file.data, cat_file.size, &cat_startup);
    TEST_REPORT("SANDBOX", "Spawn sandboxed cat with FileCap", cat_proc.success);

    if (cat_proc.success) {
        oz::sys_cspace_cap_insert(cat_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
        oz::sys_cspace_cap_insert(cat_proc.cspace_cap, 2, dummy_file_send, static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::WRITE | oz::CapRights::GRANT));

        std::uint32_t cat_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
        oz::sys_baton_inject(cat_baton, 100000);
        oz::sys_tcb_bind_baton(cat_proc.tcb_cap, cat_baton);
        oz::sys_tcb_resume(cat_proc.tcb_cap);

        // Run cat: since dummy_file_send has no responses, cat will attempt to read and exit/terminate
        oz::SchedResult res = oz::sys_baton_switch(cat_baton);
        bool cat_ran = (res.reason != oz::SchedReason::TIMEOUT && res.reason != oz::SchedReason::NONE);
        TEST_REPORT("SANDBOX", "Sandboxed cat executed under restricted CSpace", cat_ran);

        oz::sys_cap_delete(cat_baton);
    }

    oz::sys_cap_delete(cons_send);
    oz::sys_cap_delete(cons_recv);
    oz::sys_cap_delete(dummy_file_send);
    oz::sys_cap_delete(dummy_file_recv);
}

} // namespace oz::sandboxtest

extern "C" void _start() {
    oz::sys_debug_print("\n\r========================================================\n\r");
    oz::sys_debug_print(" [OZ_OS] Sandboxing & Manifest Test Runner (sandboxtest)\n\r");
    oz::sys_debug_print("========================================================\n\r");

    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_BOOTINFO, oz::sandboxtest::BOOTINFO_VADDR, 0x00, 4096) != 0) {
        oz::test::report("SANDBOX", "Map BootInfo", false);
        oz::test::exit_qemu();
    }
    const volatile auto* boot_info = reinterpret_cast<const volatile oz::BootInfo*>(oz::sandboxtest::BOOTINFO_VADDR);
    std::uint64_t initrd_size = boot_info->initrd_size;

    if (initrd_size > 0) {
        oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_INITRD, oz::sandboxtest::INITRD_VADDR, 0x00, initrd_size);
    }
    const std::uint8_t* initrd_ptr = reinterpret_cast<const std::uint8_t*>(oz::sandboxtest::INITRD_VADDR + boot_info->initrd_offset);

    oz::sandboxtest::run_manifest_sandboxing_tests(initrd_ptr, initrd_size);
    oz::sandboxtest::run_cat_sandboxing_tests(initrd_ptr, initrd_size);

    oz::test::print_summary("SANDBOX");
    oz::test::exit_qemu();
}
