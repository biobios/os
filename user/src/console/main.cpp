#include "syscall.hpp"

static constexpr std::uint32_t CAP_CONSOLE_RECV = 1;
static constexpr std::uint32_t CAP_DISPLAY_SEND = 2;

extern "C" void _start() {
    oz::sys_debug_print("Console Server started!\n\r");
    
    oz::IPCMessage msg;
    while (true) {
        if (oz::sys_channel_recv(CAP_CONSOLE_RECV, &msg)) {
            alignas(8) char buf[33];
            for (int i = 0; i < 4; ++i) {
                *reinterpret_cast<std::uint64_t*>(&buf[i * 8]) = msg.data[i];
            }
            buf[32] = '\0';
            // Output to serial log
            oz::sys_debug_print(buf);

            // Forward to Display Server for GUI rendering
            oz::sys_channel_send(CAP_DISPLAY_SEND, &msg);
        }
    }
}
