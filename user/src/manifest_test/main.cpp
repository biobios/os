#include "syscall.hpp"
#include "../common/ipc/StartupInfo.hpp"

// Declare static permissions required by this application
OZ_MANIFEST((oz::ProcessManifest{
    .magic = oz::ProcessManifest::MAGIC,
    .version = 1,
    .provides = 0,
    .required = static_cast<std::uint32_t>(oz::ServiceId::CONSOLE),
    .device_needed = oz::DeviceRequirement::NONE,
    .needs_untyped = false,
    .default_data_dir = {}
}));

OZ_PROCESS_ENTRY() {
    oz::LocalNamespace::init(info);

    oz::sys_debug_print("[ManifestTest] Process started with Local Namespace!\n\r");

    if (!info) {
        oz::sys_debug_print("[ManifestTest] FAIL: ProcessStartupInfo is null!\n\r");
        oz::sys_exit(1);
    }

    if (info->magic != oz::ProcessStartupInfo::MAGIC) {
        oz::sys_debug_print("[ManifestTest] FAIL: ProcessStartupInfo magic mismatch!\n\r");
        oz::sys_exit(1);
    }

    // Test 1: Resolve STDOUT capability by Role
    std::uint32_t stdout_cap = oz::LocalNamespace::get_cap(oz::CapRole::STDOUT);
    if (stdout_cap == 0) {
        oz::sys_debug_print("[ManifestTest] FAIL: STDOUT capability missing from local namespace!\n\r");
        oz::sys_exit(1);
    }
    oz::sys_debug_print("[ManifestTest] STDOUT capability resolved successfully by role.\n\r");

    // Test 2: Resolve STDOUT capability by Name
    std::uint32_t stdout_by_name = oz::LocalNamespace::get_cap("stdout");
    if (stdout_by_name != stdout_cap) {
        oz::sys_debug_print("[ManifestTest] FAIL: STDOUT lookup by name mismatched role slot!\n\r");
        oz::sys_exit(1);
    }
    oz::sys_debug_print("[ManifestTest] STDOUT capability resolved successfully by name.\n\r");

    // Test 3: Verify unrequested capabilities are NOT granted (Sandboxing & Principle of Least Privilege)
    std::uint32_t root_dir_cap = oz::LocalNamespace::get_cap(oz::CapRole::ROOT_DIR);
    if (root_dir_cap != 0) {
        oz::sys_debug_print("[ManifestTest] FAIL: ROOT_DIR capability leaked into sandboxed process!\n\r");
        oz::sys_exit(1);
    }
    oz::sys_debug_print("[ManifestTest] Sandboxing verified: ROOT_DIR cap is NOT present (PASS)\n\r");

    std::uint32_t untyped_cap = oz::LocalNamespace::get_cap(oz::CapRole::UNTYPED);
    if (untyped_cap != 0) {
        oz::sys_debug_print("[ManifestTest] FAIL: UNTYPED capability leaked into sandboxed process!\n\r");
        oz::sys_exit(1);
    }
    oz::sys_debug_print("[ManifestTest] Sandboxing verified: UNTYPED cap is NOT present (PASS)\n\r");

    std::uint32_t disp_cap = oz::LocalNamespace::get_cap(oz::CapRole::DISPLAY);
    if (disp_cap != 0) {
        oz::sys_debug_print("[ManifestTest] FAIL: DISPLAY capability leaked into sandboxed process!\n\r");
        oz::sys_exit(1);
    }
    oz::sys_debug_print("[ManifestTest] Sandboxing verified: DISPLAY cap is NOT present (PASS)\n\r");

    // ========================================================================
    // Step 3 Tests: Command-Line Arguments & Powerbox Capability Delegation
    // ========================================================================
    std::uint32_t argc = oz::LocalNamespace::get_argc();
    oz::sys_debug_print("[ManifestTest] argc = ");
    char num_buf[4] = {static_cast<char>('0' + (argc % 10)), '\n', '\r', '\0'};
    oz::sys_debug_print(num_buf);

    for (std::uint32_t i = 0; i < argc; ++i) {
        const char* arg = oz::LocalNamespace::get_argv(i);
        oz::sys_debug_print("[ManifestTest] argv[");
        char idx_buf[8] = {static_cast<char>('0' + i), ']', ':', ' ', '\0'};
        oz::sys_debug_print(idx_buf);
        oz::sys_debug_print(arg ? arg : "(null)");
        oz::sys_debug_print("\n\r");

        // Test 5: Check if this argument corresponds to a Powerbox delegated capability
        if (arg) {
            std::uint32_t delegated_slot = oz::LocalNamespace::get_cap(arg);
            if (delegated_slot != 0) {
                oz::sys_debug_print("[ManifestTest] PASS: Found Powerbox delegated capability for '");
                oz::sys_debug_print(arg);
                oz::sys_debug_print("'\n\r");
            }
        }
    }

    // Verify unauthorized file capability lookup returns 0 (POLA)
    std::uint32_t unauth_cap = oz::LocalNamespace::get_cap("unauthorized_secret.txt");
    if (unauth_cap != 0) {
        oz::sys_debug_print("[ManifestTest] FAIL: Unauthorized file capability leaked!\n\r");
        oz::sys_exit(1);
    }
    oz::sys_debug_print("[ManifestTest] Sandboxing verified: unauthorized file cap is 0 (PASS)\n\r");

    oz::sys_debug_print("[Test] Step 3 CLI Powerbox & Argument Passing: PASSED\n\r");
    oz::sys_exit(0);
}
