#pragma once

#include "drivers/usb/USB.hpp"
#include <cstdint>
#include <cstddef>

namespace USBClassDriver {

class ClassDriver {
public:
    virtual ~ClassDriver() = default;

    // ディスクリプタを解析し、このドライバーが対応していれば必要なエンドポイントアドレスとパラメータを返す。
    // 対応していなければ 0 を返す。
    virtual std::uint8_t matchAndParseConfiguration(USB::ConfigurationDescriptor* conf_desc, std::uint16_t& out_max_packet_size, std::uint8_t& out_interval) = 0;

    // 受信したデータを処理する
    virtual void processReport(std::uint8_t* data) = 0;
};

} // namespace USBClassDriver
