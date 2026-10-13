#include "syscall.hpp"

extern "C" void _start() {
    oz::sys_debug_print("Console Server started!\n\r");
    
    oz::IPCMessage msg;
    while (true) {
        // sys_channel_recv blocks until a message arrives
        if (oz::sys_channel_recv(1, &msg)) {
            alignas(8) char buf[33];
            for(int i = 0; i < 4; ++i) {
                *reinterpret_cast<std::uint64_t*>(&buf[i * 8]) = msg.data[i];
            }
            buf[32] = '\0';
            oz::sys_debug_print(buf);
        }
    }
}
