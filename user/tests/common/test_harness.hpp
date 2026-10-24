#pragma once

#include "syscall.hpp"

namespace oz::test {

inline int g_test_passes = 0;
inline int g_test_failures = 0;

inline void int_to_str(std::uint64_t val, char* buf, std::size_t buf_size) {
    if (buf_size == 0) return;
    if (val == 0) {
        if (buf_size >= 2) {
            buf[0] = '0';
            buf[1] = '\0';
        } else {
            buf[0] = '\0';
        }
        return;
    }
    char temp[24];
    int len = 0;
    while (val > 0 && len < 23) {
        temp[len++] = static_cast<char>('0' + (val % 10));
        val /= 10;
    }
    std::size_t out_len = static_cast<std::size_t>(len);
    if (out_len >= buf_size) {
        out_len = buf_size - 1;
    }
    for (std::size_t i = 0; i < out_len; i++) {
        buf[i] = temp[len - 1 - i];
    }
    buf[out_len] = '\0';
}

inline void report(const char* suite, const char* name, bool passed,
                   const char* file = nullptr, int line = 0, const char* detail = nullptr) {
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
        if (file && line > 0) {
            oz::sys_debug_print(" (at ");
            oz::sys_debug_print(file);
            oz::sys_debug_print(":");
            char l_buf[16];
            int_to_str(static_cast<std::uint64_t>(line), l_buf, sizeof(l_buf));
            oz::sys_debug_print(l_buf);
            oz::sys_debug_print(")");
        }
        if (detail && detail[0] != '\0') {
            oz::sys_debug_print(" - ");
            oz::sys_debug_print(detail);
        }
        oz::sys_debug_print("\n\r");
        g_test_failures++;
    }
}

inline void print_summary(const char* suite) {
    oz::sys_debug_print("\n\r========================================================\n\r");
    oz::sys_debug_print(" [");
    oz::sys_debug_print(suite);
    oz::sys_debug_print(" SUMMARY]\n\r");

    char num_buf[32];

    oz::sys_debug_print(" Passes:   ");
    int_to_str(static_cast<std::uint64_t>(g_test_passes), num_buf, sizeof(num_buf));
    oz::sys_debug_print(num_buf);
    oz::sys_debug_print("\n\r");

    oz::sys_debug_print(" Failures: ");
    int_to_str(static_cast<std::uint64_t>(g_test_failures), num_buf, sizeof(num_buf));
    oz::sys_debug_print(num_buf);
    oz::sys_debug_print("\n\r");

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

#define TEST_REPORT(suite, name, cond) oz::test::report(suite, name, (cond), __FILE__, __LINE__)
#define TEST_REPORT_DETAIL(suite, name, cond, detail) oz::test::report(suite, name, (cond), __FILE__, __LINE__, (detail))
#define TEST_ASSERT(suite, name, cond) do { \
    bool _c = (cond); \
    oz::test::report(suite, name, _c, __FILE__, __LINE__); \
    if (!_c) return; \
} while (0)
