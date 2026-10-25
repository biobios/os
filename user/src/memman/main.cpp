#include "syscall.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"

// Memory Manager requires UntypedMemory to serve system allocations
OZ_MANIFEST((oz::ProcessManifest{
    .magic = oz::ProcessManifest::MAGIC,
    .version = 1,
    .provides = 0,
    .required = static_cast<std::uint32_t>(oz::ServiceId::CONSOLE),
    .device_needed = oz::DeviceRequirement::NONE,
    .needs_untyped = true,
    .default_data_dir = {}
}));

static constexpr std::uint32_t CAP_CONSOLE_WRITE = 1;
static constexpr std::uint32_t CAP_MEMMAN_RECV   = 2;
static constexpr std::uint32_t CAP_MEMMAN_REPLY  = 3;

extern "C" void _start() {
    oz::sys_write("[MemMan] Memory Manager server started (Ring 3)!\n\r");
    oz::sys_write("[MemMan] Dedicated UntypedMemory manager active.\n\r");

    oz::ReplyToken token = oz::INVALID_REPLY_TOKEN;
    oz::IPCMessage resp{};
    oz::IPCMessage req{};

    while (true) {
        token = oz::sys_reply_recv(token, CAP_MEMMAN_RECV, (token != oz::INVALID_REPLY_TOKEN) ? &resp : nullptr, &req);
        if (token == oz::INVALID_REPLY_TOKEN) {
            continue;
        }

        auto cmd = static_cast<oz::ipc::MemCmd>(req.data[0]);
        resp = oz::IPCMessage{};

        if (cmd == oz::ipc::MemCmd::ALLOC_UNTYPED) {
            std::size_t size = static_cast<std::size_t>(req.data[1]);
            if (size == 0) size = 2 * 1024 * 1024;

            std::uint32_t child_untyped = oz::sys_untyped_retype(
                oz::CAP_UNTYPED,
                static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory),
                size
            );

            if (child_untyped == oz::INVALID_CAP_ID) {
                for (std::uint32_t pool = 200; pool < 240; ++pool) {
                    child_untyped = oz::sys_untyped_retype(
                        pool,
                        static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory),
                        size
                    );
                    if (child_untyped != oz::INVALID_CAP_ID) break;
                }
            }

            if (child_untyped != oz::INVALID_CAP_ID) {
                resp.data[0] = 0; // Success
                resp.has_cap = true;
                resp.transferred_cap = child_untyped;
            } else {
                resp.data[0] = 1; // Out of memory / failure
                resp.has_cap = false;
                oz::sys_write("[MemMan] ERROR: Failed to allocate child UntypedMemory!\n\r");
            }

        } else if (cmd == oz::ipc::MemCmd::REVOKE_UNTYPED) {
            if (req.has_cap && req.transferred_cap != oz::INVALID_CAP_ID) {
                oz::sys_untyped_revoke(req.transferred_cap);
                oz::sys_cap_delete(req.transferred_cap);
                resp.data[0] = 0; // Success
            } else {
                resp.data[0] = 1; // Error: missing capability
            }
            resp.has_cap = false;

        } else if (cmd == oz::ipc::MemCmd::GET_INFO) {
            resp.data[0] = 0;
            resp.data[1] = 0; // Total
            resp.data[2] = 0; // Free
            resp.has_cap = false;

        } else {
            resp.data[0] = 0xFF; // Unknown command
            resp.has_cap = false;
        }
    }
}
