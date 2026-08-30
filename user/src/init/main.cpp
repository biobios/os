#include "syscall.hpp"

extern "C" void _start() {
    oz::sys_write("Hello from init process (Ring 3)!\n\r");
    
    // Some dummy loop or exit
    for (int i = 0; i < 3; ++i) {
        oz::sys_write("Running user code...\n\r");
    }
    
    oz::sys_exit(0);
}
