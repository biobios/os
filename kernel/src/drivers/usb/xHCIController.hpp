#pragma once

#include "drivers/usb/xHCI.hpp"
#include "drivers/usb/xHCIRing.hpp"
#include "drivers/pci/PCIUtils.hpp"
#include "memory/FrameManager.hpp"
#include "drivers/usb/USB.hpp"
#include "drivers/keyboard/HIDKeyboard.hpp"
#include <cstdint>

namespace xHCI {

enum class DeviceState : std::uint8_t {
    Blank = 0,
    Addressed = 1,
    GettingDeviceDescriptor = 2,
    GettingConfigHeader = 3,
    GettingFullConfig = 4,
    ParsingConfig = 5,
    ConfiguringEndpoint = 6,
    SettingConfiguration = 8,
    Running = 7
};

struct Device {
    Ring transfer_rings[31];
    void* control_buffer;
    InputContext* input_context;
    std::uint8_t* report_buffer;
    std::uint8_t dci_interrupt_in;
    std::uint8_t config_value;
    DeviceState state{DeviceState::Blank};
};

class Controller {
public:
    Controller(PCIUtils::PCIFunction pci_function);
    
    // コントローラの初期化（MMIOのマッピング、データ構造の割り当て、xHCの起動）
    bool initialize(oz::x86_64::FrameManager& fm);
    
    // イベントリングをポーリングし、発生したイベントを処理する
    void processEvents();
    
    // ポートの状態変化を監視し、デバイス接続時にリセットを発行する
    void pollPorts();

private:
    PCIUtils::PCIFunction pci_function_;
    CapabilityRegisters volatile* cap_regs_;
    OperationalRegisters volatile* op_regs_;
    RuntimeRegisters volatile* rt_regs_;
    std::uint32_t volatile* doorbell_regs_;

    Ring command_ring_;
    EventRing event_ring_;

    std::uint64_t* dcbaa_; // Device Context Base Address Array
    std::uint8_t max_ports_;
    oz::x86_64::FrameManager* fm_;
    
    Device devices_[256];
    HID::Keyboard keyboard_;
    
    // ヘルパー関数
    void reset();
    void ringDoorbell(std::uint8_t target, std::uint8_t stream_id = 0);
    void issueEnableSlotCommand();
    void issueAddressDeviceCommand(std::uint8_t slot_id);
    void issueGetDescriptor(std::uint8_t slot_id, std::uint8_t desc_type, std::uint8_t desc_index, std::uint16_t length);
    void parseConfigurationDescriptor(std::uint8_t slot_id);
    void issueConfigureEndpointCommand(std::uint8_t slot_id, std::uint8_t endpoint_address, std::uint16_t max_packet_size, std::uint8_t interval);
    void issueSetConfiguration(std::uint8_t slot_id);
    void issueKeyboardTransfer(std::uint8_t slot_id);
};

} // namespace xHCI
