#pragma once

#include "drivers/usb/xhci/xHCI.hpp"
#include "drivers/pci/PCIUtils.hpp"
#include "memory/FrameManager.hpp"
#include "drivers/usb/USB.hpp"
#include "drivers/usb/class/ClassDriver.hpp"
#include <cstdint>
#include <cstddef>

namespace xHCIUtils {

class Ring {
public:
    Ring() : buf_(nullptr), ring_size_(0), cycle_bit_(1), enqueue_index_(0) {}
    
    void initialize(xHCI::TRB::Any volatile* buf, std::size_t size);
    void push(const xHCI::TRB::Any& trb);
    
    xHCI::TRB::Any volatile* getBuffer() const { return buf_; }
    std::uint32_t getCycleBit() const { return cycle_bit_; }
    
private:
    xHCI::TRB::Any volatile* buf_;
    std::size_t ring_size_;
    std::uint32_t cycle_bit_;
    std::size_t enqueue_index_;
};

class EventRing {
public:
    EventRing() : buf_(nullptr), ring_size_(0), cycle_bit_(1), dequeue_index_(0) {}
    
    void initialize(xHCI::TRB::Any volatile* buf, std::size_t size);
    bool hasEvent();
    xHCI::TRB::Any pop();
    std::size_t getDequeueIndex() const { return dequeue_index_; }
    xHCI::TRB::Any volatile* getBuffer() const { return buf_; }

private:
    xHCI::TRB::Any volatile* buf_;
    std::size_t ring_size_;
    std::uint32_t cycle_bit_;
    std::size_t dequeue_index_;
};

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
    xHCI::InputContext* input_context;
    std::uint8_t* report_buffer;
    std::uint8_t dci_interrupt_in;
    std::uint8_t config_value;
    USBClassDriver::ClassDriver* driver{nullptr};
    DeviceState state{DeviceState::Blank};
};

class Controller {
public:
    Controller(PCIUtils::PCIFunction pci_function);
    
    bool initialize(oz::x86_64::FrameManager& fm);
    void processEvents();
    void pollPorts();
    void registerClassDriver(USBClassDriver::ClassDriver* driver);

private:
    PCIUtils::PCIFunction pci_function_;
    xHCI::CapabilityRegisters volatile* cap_regs_;
    xHCI::OperationalRegisters volatile* op_regs_;
    xHCI::RuntimeRegisters volatile* rt_regs_;
    std::uint32_t volatile* doorbell_regs_;

    Ring command_ring_;
    EventRing event_ring_;

    std::uint64_t* dcbaa_; 
    std::uint8_t max_ports_;
    oz::x86_64::FrameManager* fm_;
    
    Device devices_[256];
    USBClassDriver::ClassDriver* class_drivers_[16];
    int num_class_drivers_{0};
    
    void reset();
    void ringDoorbell(std::uint8_t target, std::uint8_t stream_id = 0);
    void issueEnableSlotCommand();
    void issueAddressDeviceCommand(std::uint8_t slot_id);
    void issueGetDescriptor(std::uint8_t slot_id, std::uint8_t desc_type, std::uint8_t desc_index, std::uint16_t length);
    void parseConfigurationDescriptor(std::uint8_t slot_id);
    void issueConfigureEndpointCommand(std::uint8_t slot_id, std::uint8_t endpoint_address, std::uint16_t max_packet_size, std::uint8_t interval);
    void issueSetConfiguration(std::uint8_t slot_id);
    void issueInterruptTransfer(std::uint8_t slot_id);
};

} // namespace xHCIUtils
