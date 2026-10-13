#pragma once

#include "xHCI.hpp"
#include "USB.hpp"
#include <cstdint>
#include <cstddef>

namespace xHCIDriver {

class Ring {
public:
    Ring() : buf_(nullptr), buf_phys_(0), ring_size_(0), cycle_bit_(1), enqueue_index_(0) {}
    
    void initialize(xHCI::TRB::Any volatile* buf, std::uint64_t buf_phys, std::size_t size);
    void push(const xHCI::TRB::Any& trb);
    
    xHCI::TRB::Any volatile* getBuffer() const { return buf_; }
    std::uint32_t getCycleBit() const { return cycle_bit_; }
    
private:
    xHCI::TRB::Any volatile* buf_;
    std::uint64_t buf_phys_;
    std::size_t ring_size_;
    std::uint32_t cycle_bit_;
    std::size_t enqueue_index_;
};

class EventRing {
public:
    EventRing() : buf_(nullptr), buf_phys_(0), ring_size_(0), cycle_bit_(1), dequeue_index_(0) {}
    
    void initialize(xHCI::TRB::Any volatile* buf, std::uint64_t buf_phys, std::size_t size);
    bool hasEvent();
    xHCI::TRB::Any pop();
    std::size_t getDequeueIndex() const { return dequeue_index_; }
    xHCI::TRB::Any volatile* getBuffer() const { return buf_; }
    std::uint64_t getPhysicalAddress() const { return buf_phys_; }

private:
    xHCI::TRB::Any volatile* buf_;
    std::uint64_t buf_phys_;
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
    void* control_buffer{nullptr};
    xHCI::InputContext* input_context{nullptr};
    std::uint8_t* report_buffer{nullptr};
    std::uint8_t dci_interrupt_in{0};
    std::uint8_t config_value{0};
    DeviceState state{DeviceState::Blank};
    std::uint64_t device_context_phys{0};
    void* device_context_virt{nullptr};
};

struct DmaPage {
    void* virt;
    std::uint64_t phys;
};

using ReportCallback = void (*)(const std::uint8_t*);

class Controller {
public:
    Controller(std::uint64_t mmio_virt);
    
    bool initialize();
    void processEvents();
    void scanInitialPorts();
    void handlePortStatusChange(std::uint8_t port_id);
    void clearEventInterruptStatus();
    void setReportCallback(ReportCallback cb) { report_callback_ = cb; }

private:
    std::uint64_t mmio_virt_;
    xHCI::CapabilityRegisters volatile* cap_regs_;
    xHCI::OperationalRegisters volatile* op_regs_;
    xHCI::RuntimeRegisters volatile* rt_regs_;
    std::uint32_t volatile* doorbell_regs_;

    Ring command_ring_;
    EventRing event_ring_;

    std::uint64_t* dcbaa_; 
    std::uint8_t max_ports_;
    
    Device devices_[256];
    ReportCallback report_callback_{nullptr};
    
    DmaPage allocPage();
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

} // namespace xHCIDriver
