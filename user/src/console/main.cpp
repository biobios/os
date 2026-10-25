#include "syscall.hpp"
#include "../common/ipc/StartupInfo.hpp"

OZ_MANIFEST((oz::ProcessManifest{
    .magic = oz::ProcessManifest::MAGIC,
    .version = 1,
    .provides = static_cast<std::uint32_t>(oz::ServiceId::CONSOLE),
    .required = static_cast<std::uint32_t>(oz::ServiceId::DISPLAY),
    .device_needed = oz::DeviceRequirement::NONE,
    .needs_untyped = false,
    .default_data_dir = {}
}));

static constexpr std::uint32_t CAP_CONSOLE_RECV = 1;
static constexpr std::uint32_t CAP_DISPLAY_SEND = 2;

extern "C" void _start() {
    oz::sys_debug_print("Console Server started!\n\r");
    
    oz::ReplyToken token = oz::INVALID_REPLY_TOKEN;
    oz::IPCMessage reply{};
    oz::IPCMessage msg{};

    while (true) {
        token = oz::sys_reply_recv(token, CAP_CONSOLE_RECV, (token != oz::INVALID_REPLY_TOKEN) ? &reply : nullptr, &msg);
        if (token == oz::INVALID_REPLY_TOKEN) {
            continue;
        }

        alignas(8) char buf[33];
        for (int i = 0; i < 4; ++i) {
            *reinterpret_cast<std::uint64_t*>(&buf[i * 8]) = msg.data[i];
        }
        buf[32] = '\0';
        // Output to serial log
        oz::sys_debug_print(buf);

        // Forward to Display Server for GUI rendering
        oz::IPCMessage disp_resp{};
        oz::sys_call(CAP_DISPLAY_SEND, &msg, &disp_resp);

        reply.data[0] = 0; // Success ACK
    }
}
