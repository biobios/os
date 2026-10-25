#include "syscall.hpp"
#include "../common/ipc/StartupInfo.hpp"
#include "../../lib/io.hpp"

// Sandboxed Process Manifest: No ambient VFS root authority
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

    auto stdout_h = oz::io::FileHandle::from_role(oz::CapRole::STDOUT);
    oz::io::FileHandle in_h{};

    std::uint32_t argc = oz::LocalNamespace::get_argc();
    if (argc > 1) {
        // Read from delegated capability specified in arguments
        const char* target_name = oz::LocalNamespace::get_argv(1);
        in_h = oz::io::FileHandle::from_name(target_name);
        if (!in_h.is_valid()) {
            // Try resolving by role if not found by name
            in_h = oz::io::FileHandle::from_role(oz::CapRole::STDIN);
        }
    } else {
        // Read from standard input (STDIN capability)
        in_h = oz::io::FileHandle::from_role(oz::CapRole::STDIN);
    }

    if (!in_h.is_valid()) {
        oz::sys_write("cat: No valid input capability provided\n\r");
        oz::sys_exit(1);
    }

    char buf[64];
    while (true) {
        std::size_t n = in_h.read(buf, sizeof(buf));
        if (n == 0) break; // EOF
        stdout_h.write(buf, n);
    }

    oz::sys_exit(0);
}
