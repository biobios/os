#pragma once

#include "syscall.hpp"

namespace oz::test {

inline int g_test_passes = 0;
inline int g_test_failures = 0;

inline void report(const char* suite, const char* name, bool passed) {
    if (passed) {
        oz::sys_debug_print("[");
        oz::sys_debug_print(suite);
        oz::sys_debug_print("][PASS] ");
        oz::sys_debug_print(name);
        oz::sys_debug_print("\n\r");
        g_test_passes++;
    } else {
        oz::sys_debug_print("[");
        oz::sys_debug_print(suite);
        oz::sys_debug_print("][FAIL] ");
        oz::sys_debug_print(name);
        oz::sys_debug_print("\n\r");
        g_test_failures++;
    }
}

inline void print_summary(const char* suite) {
    oz::sys_debug_print("\n\r========================================================\n\r");
    oz::sys_debug_print(" [");
    oz::sys_debug_print(suite);
    oz::sys_debug_print(" SUMMARY]\n\r");

    oz::sys_debug_print(" Passes:   ");
    char p_buf[4] = {static_cast<char>('0' + (g_test_passes / 10)),
                     static_cast<char>('0' + (g_test_passes % 10)), '\n', '\r'};
    oz::sys_debug_print(p_buf);

    oz::sys_debug_print(" Failures: ");
    char f_buf[4] = {static_cast<char>('0' + (g_test_failures / 10)),
                     static_cast<char>('0' + (g_test_failures % 10)), '\n', '\r'};
    oz::sys_debug_print(f_buf);

    if (g_test_failures == 0) {
        oz::sys_debug_print("\n\r >>> ALL TESTS PASSED! <<<\n\r");
    } else {
        oz::sys_debug_print("\n\r >>> SOME TESTS FAILED! <<<\n\r");
    }
    oz::sys_debug_print("========================================================\n\r");
}

inline void exit_qemu() {
    // 0x10 -> exit code 33 (success)
    // 0x11 -> exit code 35 (failure)
    std::uint8_t exit_val = (g_test_failures == 0) ? 0x10 : 0x11;
    oz::sys_ioport_write(oz::CAP_ROOT_IOPORT, 0xf4, exit_val, 1);
    while (true) {
        oz::sys_yield();
    }
}

} // namespace oz::test

#define TEST_REPORT(suite, name, cond) oz::test::report(suite, name, cond)
