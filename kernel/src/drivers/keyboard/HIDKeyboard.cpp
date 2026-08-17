#include "drivers/keyboard/HIDKeyboard.hpp"
#include "utils/utils.hpp" // For dprint

namespace HID {

static const char keycode_map[] = {
    0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', // 0x00 - 0x10
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', // 0x11 - 0x1D
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', // 0x1E - 0x27
    '\n', 0, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/' // 0x28 - 0x38
};

static const char shift_keycode_map[] = {
    0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', // 0x00 - 0x10
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', // 0x11 - 0x1D
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', // 0x1E - 0x27
    '\n', 0, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?' // 0x28 - 0x38
};

Keyboard::Keyboard() {}

char Keyboard::keycodeToAscii(std::uint8_t keycode, bool shift) {
    if (keycode < sizeof(keycode_map)) {
        return shift ? shift_keycode_map[keycode] : keycode_map[keycode];
    }
    return 0;
}

void Keyboard::processReport(const std::uint8_t* report) {
    bool shift_pressed = (report[0] & 0x22) != 0; // LShift (bit 1) or RShift (bit 5)

    // 現在のレポートで押されているキーを探す
    for (int i = 2; i < 8; ++i) {
        std::uint8_t key = report[i];
        if (key == 0) continue;

        // 以前のレポートに含まれていないキーのみ処理する（押しっぱなしの連打防止）
        bool is_new_key = true;
        for (int j = 2; j < 8; ++j) {
            if (prev_report_[j] == key) {
                is_new_key = false;
                break;
            }
        }

        if (is_new_key) {
            char ascii = keycodeToAscii(key, shift_pressed);
            if (ascii) {
                // シンプルに dprint で1文字出力する
                char str[2] = {ascii, '\0'};
                dprint(str);
            }
        }
    }

    // 前回のレポートを保存
    for (int i = 0; i < 8; ++i) {
        prev_report_[i] = report[i];
    }
}

} // namespace HID
