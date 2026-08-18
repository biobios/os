#include "drivers/usb/xhci/xHCIUtils.hpp"
#include "memory/Address.hpp"
#include "utils/utils.hpp"

namespace xHCIUtils {

// --- Ring Implementation ---
void Ring::initialize(xHCI::TRB::Any volatile* buf, std::size_t size) {
    buf_ = buf;
    ring_size_ = size;
    cycle_bit_ = 1;
    enqueue_index_ = 0;
}

void Ring::push(const xHCI::TRB::Any& trb) {
    xHCI::TRB::Any volatile* target = &buf_[enqueue_index_];
    target->data[0] = trb.data[0];
    target->data[1] = trb.data[1];
    target->data[2] = trb.data[2];
    
    std::uint32_t data3 = trb.data[3] & ~1;
    data3 |= cycle_bit_;
    target->data[3] = data3;

    enqueue_index_++;
    if (enqueue_index_ == ring_size_ - 1) {
        xHCI::TRB::Any volatile* link_trb = &buf_[enqueue_index_];
        link_trb->data[3] = (link_trb->data[3] & ~1) | cycle_bit_;
        if ((link_trb->data[3] & 2) != 0) {
            cycle_bit_ ^= 1;
        }
        enqueue_index_ = 0;
    }
}

// --- EventRing Implementation ---
void EventRing::initialize(xHCI::TRB::Any volatile* buf, std::size_t size) {
    buf_ = buf;
    ring_size_ = size;
    cycle_bit_ = 1;
    dequeue_index_ = 0;
}

bool EventRing::hasEvent() {
    return (buf_[dequeue_index_].data[3] & 1) == cycle_bit_;
}

xHCI::TRB::Any EventRing::pop() {
    xHCI::TRB::Any event;
    event.data[0] = buf_[dequeue_index_].data[0];
    event.data[1] = buf_[dequeue_index_].data[1];
    event.data[2] = buf_[dequeue_index_].data[2];
    event.data[3] = buf_[dequeue_index_].data[3];
    
    dequeue_index_++;
    if (dequeue_index_ == ring_size_) {
        dequeue_index_ = 0;
        cycle_bit_ ^= 1;
    }
    return event;
}

// --- Controller Implementation ---
Controller::Controller(PCIUtils::PCIFunction pci_function) 
    : pci_function_(pci_function), cap_regs_(nullptr), op_regs_(nullptr), rt_regs_(nullptr), doorbell_regs_(nullptr), dcbaa_(nullptr) {}

bool Controller::initialize(oz::x86_64::FrameManager& fm) {
    fm_ = &fm;
    dprint("[xHCI] Starting Init0\r\n");
    
    pci_function_.header->Command |= 0b110;
    
    dprint("[xHCI] PCI Command Reg: ");
    char hex_str_cmd[17];
    oz::utils::to_hex(pci_function_.header->Command, hex_str_cmd);
    dprint(hex_str_cmd);
    dprint("\r\n");
    
    dprint("[xHCI] Starting Init1\r\n");
    
    std::uint32_t bar0 = pci_function_.header->BaseAddressRegister[0];
    std::uint32_t bar1 = pci_function_.header->BaseAddressRegister[1];
    dprint("[xHCI] Starting Init2\r\n");
    
    std::uintptr_t mmio_base = bar0 & ~0xFULL;
    if ((bar0 & 0b110) == 0b100) {
        mmio_base |= static_cast<std::uint64_t>(bar1) << 32;
    }
    
    char hex_str[17];
    oz::utils::to_hex(mmio_base, hex_str);
    dprint("[xHCI] MMIO Base: 0x");
    dprint(hex_str);
    dprint("\r\n");
    
    cap_regs_ = reinterpret_cast<xHCI::CapabilityRegisters volatile*>(mmio_base + oz::DIRECT_MAP_OFFSET);
    op_regs_ = reinterpret_cast<xHCI::OperationalRegisters volatile*>(reinterpret_cast<std::uintptr_t>(cap_regs_) + cap_regs_->capLength);
    rt_regs_ = reinterpret_cast<xHCI::RuntimeRegisters volatile*>(reinterpret_cast<std::uintptr_t>(cap_regs_) + (cap_regs_->rtsoff & ~0x1F));
    doorbell_regs_ = reinterpret_cast<std::uint32_t volatile*>(reinterpret_cast<std::uintptr_t>(cap_regs_) + (cap_regs_->dboff & ~0x3));

    dprint("[xHCI] DBOFF: ");
    char hex_str_dboff[17];
    oz::utils::to_hex(cap_regs_->dboff, hex_str_dboff);
    dprint(hex_str_dboff);
    dprint(", RTSOFF: ");
    oz::utils::to_hex(cap_regs_->rtsoff, hex_str_dboff);
    dprint(hex_str_dboff);
    dprint("\r\n");

    dprint("[xHCI] Starting Init3\r\n");
    
    std::uint32_t max_scratchpads = ((cap_regs_->hcsParams2 >> 21) & 0x1F) | (((cap_regs_->hcsParams2 >> 27) & 0x1F) << 5);
    
    dprint("[xHCI] Max Scratchpad Buffers: ");
    char hex_str_sp[17];
    oz::utils::to_hex(max_scratchpads, hex_str_sp);
    dprint(hex_str_sp);
    dprint("\r\n");
    
    max_ports_ = static_cast<std::uint8_t>(cap_regs_->hcsParams1 >> 24);

    dprint("[xHCI] Starting Reset\r\n");
    reset();
    dprint("[xHCI] Reset Done\r\n");

    dprint("[xHCI] Allocating DCBAA\r\n");
    oz::PageBlock<> dcbaa_block = fm.allocateBlock(0);
    dcbaa_ = reinterpret_cast<std::uint64_t*>(oz::phys_to_virt(fm.getPhysicalAddress(dcbaa_block)));
    for(int i = 0; i < 256; i++) dcbaa_[i] = 0;

    std::uint32_t hcsparams2 = cap_regs_->hcsParams2;
    std::uint32_t max_scratchpad = ((hcsparams2 >> 21) & 0x1F) | (((hcsparams2 >> 27) & 0x1F) << 5);
    
    if (max_scratchpad > 0) {
        dprint("[xHCI] Allocating Scratchpad Buffers\r\n");
        oz::PageBlock<> scratch_array_block = fm.allocateBlock(0);
        std::uint64_t* scratch_array = reinterpret_cast<std::uint64_t*>(oz::phys_to_virt(fm.getPhysicalAddress(scratch_array_block)));
        for (std::uint32_t i = 0; i < max_scratchpad; ++i) {
            oz::PageBlock<> buffer = fm.allocateBlock(0);
            scratch_array[i] = fm.getPhysicalAddress(buffer).get();
        }
        dcbaa_[0] = fm.getPhysicalAddress(scratch_array_block).get();
    }
    
    dprint("[xHCI] Allocating Command Ring\r\n");
    oz::PageBlock<> cmd_block = fm.allocateBlock(0);
    xHCI::TRB::Any* cmd_buf = reinterpret_cast<xHCI::TRB::Any*>(oz::phys_to_virt(fm.getPhysicalAddress(cmd_block)));
    std::size_t cmd_ring_size = 4096 / sizeof(xHCI::TRB::Any);
    for (std::size_t i = 0; i < cmd_ring_size; ++i) {
        cmd_buf[i].data[0] = 0; cmd_buf[i].data[1] = 0;
        cmd_buf[i].data[2] = 0; cmd_buf[i].data[3] = 0;
    }
    command_ring_.initialize(cmd_buf, cmd_ring_size);
    cmd_buf[cmd_ring_size - 1].data[0] = static_cast<std::uint32_t>(fm.getPhysicalAddress(cmd_block).get());
    cmd_buf[cmd_ring_size - 1].data[1] = static_cast<std::uint32_t>(fm.getPhysicalAddress(cmd_block).get() >> 32);
    cmd_buf[cmd_ring_size - 1].data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::Link) << 10) | 2;

    dprint("[xHCI] Allocating Event Ring\r\n");
    oz::PageBlock<> event_block = fm.allocateBlock(0);
    xHCI::TRB::Any* event_buf = reinterpret_cast<xHCI::TRB::Any*>(oz::phys_to_virt(fm.getPhysicalAddress(event_block)));
    std::size_t event_ring_size = 4096 / sizeof(xHCI::TRB::Any);
    for (std::size_t i = 0; i < event_ring_size; ++i) {
        event_buf[i].data[0] = 0; event_buf[i].data[1] = 0;
        event_buf[i].data[2] = 0; event_buf[i].data[3] = 0;
    }
    event_ring_.initialize(event_buf, event_ring_size);

    dprint("[xHCI] Allocating ERST\r\n");
    oz::PageBlock<> erst_block = fm.allocateBlock(0);
    xHCI::EventRingSegmentTableEntry* erst = reinterpret_cast<xHCI::EventRingSegmentTableEntry*>(oz::phys_to_virt(fm.getPhysicalAddress(erst_block)));
    erst[0].RingSegmentBaseAddressLo = static_cast<std::uint32_t>(fm.getPhysicalAddress(event_block).get());
    erst[0].RingSegmentBaseAddressHi = static_cast<std::uint32_t>(fm.getPhysicalAddress(event_block).get() >> 32);
    erst[0].RingSegmentSize = event_ring_size;
    erst[0].reserved = 0;
    erst[0].reserved2 = 0;

    dprint("[xHCI] Writing to OpRegs & RtRegs\r\n");
    op_regs_->deviceContextBaseAddressArrayPointer = fm.getPhysicalAddress(dcbaa_block).get();
    
    std::uint64_t cmd_phys = fm.getPhysicalAddress(cmd_block).get();
    op_regs_->cmdRingControl = (cmd_phys & ~0x3FULL) | 1;

    rt_regs_->IR[0].EventRingSegmentTableSize = 1;
    rt_regs_->IR[0].EventRingSegmentTableBaseAddress = fm.getPhysicalAddress(erst_block).get();
    rt_regs_->IR[0].EventRingDequeuePointer = fm.getPhysicalAddress(event_block).get() | (1 << 3);

    dprint("[xHCI] Running Controller (Polling Mode - but enabling xHC INT just in case)\r\n");
    rt_regs_->IR[0].InterrupterManagement |= xHCI::IMAN::InterruptEnable;
    op_regs_->usbCommand |= xHCI::USBCommand::InterrupterEnable;
    
    std::uint8_t max_slots = cap_regs_->hcsParams1 & 0xFF;
    op_regs_->configure = max_slots;
    
    op_regs_->usbCommand |= xHCI::USBCommand::RunStop;
    while ((op_regs_->usbStatus & xHCI::USBStatus::HostControllerHalted) != 0) {}

    dprint("[xHCI] Controller Started\r\n");
    return true;
}

void Controller::reset() {
    while ((op_regs_->usbStatus & xHCI::USBStatus::ControllerNotReady) != 0) {}

    op_regs_->usbCommand |= xHCI::USBCommand::HostControllerReset;
    
    while ((op_regs_->usbCommand & xHCI::USBCommand::HostControllerReset) != 0) {}
    while ((op_regs_->usbStatus & xHCI::USBStatus::ControllerNotReady) != 0) {}
}

void Controller::processEvents() {
    while (event_ring_.hasEvent()) {
        xHCI::TRB::Any event = event_ring_.pop();
        
        std::uint8_t type = (event.data[3] >> 10) & 0x3F;
        std::uint8_t completion_code = (event.data[2] >> 24) & 0xFF;
        
        if (type == static_cast<std::uint8_t>(xHCI::TRB::Type::CommandCompletionEvent)) {
            std::uint8_t slot_id = (event.data[3] >> 24) & 0xFF;
            if (completion_code == static_cast<std::uint8_t>(xHCI::CompletionCode::Success)) {
                if (slot_id > 0) {
                    if (devices_[slot_id].state == DeviceState::Blank) {
                        issueAddressDeviceCommand(slot_id);
                    } else if (devices_[slot_id].state == DeviceState::Addressed) {
                        devices_[slot_id].state = DeviceState::GettingDeviceDescriptor;
                        issueGetDescriptor(slot_id, static_cast<std::uint8_t>(USB::DescriptorType::Device), 0, sizeof(USB::DeviceDescriptor));
                    } else if (devices_[slot_id].state == DeviceState::ConfiguringEndpoint) {
                        devices_[slot_id].state = DeviceState::SettingConfiguration;
                        issueSetConfiguration(slot_id);
                    }
                }
            } else {
                dprint("[xHCI] Command Failed Code: ");
                char hex_str[17];
                oz::utils::to_hex(completion_code, hex_str);
                dprint(hex_str);
                dprint("\r\n");
            }
        } else if (type == static_cast<std::uint8_t>(xHCI::TRB::Type::TransferEvent)) {
            std::uint8_t slot_id = (event.data[3] >> 24) & 0xFF;
            std::uint8_t completion_code = (event.data[2] >> 24) & 0xFF;
            
            if (completion_code == static_cast<std::uint8_t>(xHCI::CompletionCode::Success) || 
                completion_code == static_cast<std::uint8_t>(xHCI::CompletionCode::ShortPacket)) {
                if (slot_id > 0) {
                    if (devices_[slot_id].state == DeviceState::GettingDeviceDescriptor) {
                        devices_[slot_id].state = DeviceState::GettingConfigHeader;
                        issueGetDescriptor(slot_id, static_cast<std::uint8_t>(USB::DescriptorType::Configuration), 0, 9);
                    } else if (devices_[slot_id].state == DeviceState::GettingConfigHeader) {
                        devices_[slot_id].state = DeviceState::GettingFullConfig;
                        USB::ConfigurationDescriptor* conf_desc = reinterpret_cast<USB::ConfigurationDescriptor*>(devices_[slot_id].control_buffer);
                        std::uint16_t total_length = conf_desc->total_length;
                        issueGetDescriptor(slot_id, static_cast<std::uint8_t>(USB::DescriptorType::Configuration), 0, total_length);
                    } else if (devices_[slot_id].state == DeviceState::GettingFullConfig) {
                        devices_[slot_id].state = DeviceState::ParsingConfig;
                        parseConfigurationDescriptor(slot_id);
                    } else if (devices_[slot_id].state == DeviceState::SettingConfiguration) {
                        devices_[slot_id].state = DeviceState::Running;
                        issueKeyboardTransfer(slot_id);
                    } else if (devices_[slot_id].state == DeviceState::Running) {
                        keyboard_driver_.processReport(devices_[slot_id].report_buffer);
                        issueKeyboardTransfer(slot_id);
                    }
                }
            } else {
                dprint("[xHCI] Transfer Failed Code: ");
                char hex_str[17];
                oz::utils::to_hex(completion_code, hex_str);
                dprint(hex_str);
                dprint("\r\n");
            }
        }
        
        std::uint64_t erdp = reinterpret_cast<std::uint64_t>(const_cast<xHCI::TRB::Any*>(event_ring_.getBuffer())) - oz::DIRECT_MAP_OFFSET + event_ring_.getDequeueIndex() * sizeof(xHCI::TRB::Any);
        rt_regs_->IR[0].EventRingDequeuePointer = erdp | (1 << 3);
    }
}

void Controller::pollPorts() {
    for (std::uint8_t i = 0; i < max_ports_; ++i) {
        std::uint32_t portsc = op_regs_->ports[i].PortStatusAndControl;
        
        if ((portsc & xHCI::PORTSC::ConnectStatusChange) != 0) {
            op_regs_->ports[i].PortStatusAndControl = portsc | xHCI::PORTSC::ConnectStatusChange;
            
            if ((portsc & xHCI::PORTSC::CurrentConnectStatus) != 0) {
                std::uint32_t reset_portsc = op_regs_->ports[i].PortStatusAndControl;
                reset_portsc &= ~(xHCI::PORTSC::ConnectStatusChange | xHCI::PORTSC::PortEnabledDisabledChange | xHCI::PORTSC::OverCurrentChange | xHCI::PORTSC::PortResetChange | xHCI::PORTSC::PortLinkStateChange | xHCI::PORTSC::PortConfigErrorChange);
                reset_portsc |= xHCI::PORTSC::PortReset;
                op_regs_->ports[i].PortStatusAndControl = reset_portsc;
            }
        }
        
        if ((portsc & xHCI::PORTSC::PortResetChange) != 0) {
            op_regs_->ports[i].PortStatusAndControl = portsc | xHCI::PORTSC::PortResetChange;
            
            if ((portsc & xHCI::PORTSC::PortEnabledDisabled) != 0) {
                dprint("[xHCI] Port Enabled -> Issuing Enable Slot Command\r\n");
                issueEnableSlotCommand();
                dprint("[xHCI] Enable Slot Command Issued\r\n");
            }
        }
    }
}

void Controller::ringDoorbell(std::uint8_t target, std::uint8_t stream_id) {
    __asm__ volatile("mfence" ::: "memory");
    doorbell_regs_[target] = stream_id;
}

void Controller::issueEnableSlotCommand() {
    xHCI::TRB::Any cmd;
    cmd.data[0] = 0;
    cmd.data[1] = 0;
    cmd.data[2] = 0;
    cmd.data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::EnableSlotCommand) << 10) | (1 << 5);

    command_ring_.push(cmd);
    ringDoorbell(0);
}

void Controller::issueAddressDeviceCommand(std::uint8_t slot_id) {
    if (!fm_) return;

    oz::PageBlock<> dev_ctx_block = fm_->allocateBlock(0);
    xHCI::DeviceContext* dev_ctx = reinterpret_cast<xHCI::DeviceContext*>(oz::phys_to_virt(fm_->getPhysicalAddress(dev_ctx_block)));
    for(std::size_t i = 0; i < sizeof(xHCI::DeviceContext); ++i) reinterpret_cast<std::uint8_t*>(dev_ctx)[i] = 0;
    
    dcbaa_[slot_id] = fm_->getPhysicalAddress(dev_ctx_block).get();

    oz::PageBlock<> input_ctx_block = fm_->allocateBlock(0);
    xHCI::InputContext* input_ctx = reinterpret_cast<xHCI::InputContext*>(oz::phys_to_virt(fm_->getPhysicalAddress(input_ctx_block)));
    for(std::size_t i = 0; i < sizeof(xHCI::InputContext); ++i) reinterpret_cast<std::uint8_t*>(input_ctx)[i] = 0;
    
    input_ctx->inputControlContext.AddContextFlags = (1 << 0) | (1 << 1);

    input_ctx->slotContext.Offset00h = (1 << 27);
    input_ctx->slotContext.Offset04h = (1 << 16); 

    oz::PageBlock<> ep0_ring_block = fm_->allocateBlock(0);
    xHCI::TRB::Any* ep0_ring_buf = reinterpret_cast<xHCI::TRB::Any*>(oz::phys_to_virt(fm_->getPhysicalAddress(ep0_ring_block)));
    std::size_t ep0_ring_size = 4096 / sizeof(xHCI::TRB::Any);
    
    for (std::size_t i = 0; i < ep0_ring_size; ++i) {
        ep0_ring_buf[i].data[0] = 0; ep0_ring_buf[i].data[1] = 0;
        ep0_ring_buf[i].data[2] = 0; ep0_ring_buf[i].data[3] = 0;
    }
    
    devices_[slot_id].transfer_rings[0].initialize(ep0_ring_buf, ep0_ring_size);
    
    ep0_ring_buf[ep0_ring_size - 1].data[0] = static_cast<std::uint32_t>(fm_->getPhysicalAddress(ep0_ring_block).get());
    ep0_ring_buf[ep0_ring_size - 1].data[1] = static_cast<std::uint32_t>(fm_->getPhysicalAddress(ep0_ring_block).get() >> 32);
    ep0_ring_buf[ep0_ring_size - 1].data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::Link) << 10) | 2;
    
    input_ctx->endpointContext[0].EPState = 0; 
    input_ctx->endpointContext[0].Offset01h = 0;
    input_ctx->endpointContext[0].Offset04h = (4 << 3);
    input_ctx->endpointContext[0].MaxPacketSize = 8;
    
    std::uint64_t ep0_phys = fm_->getPhysicalAddress(ep0_ring_block).get();
    input_ctx->endpointContext[0].TRDequeuePointerLo = ep0_phys & 0xFFFFFFFF;
    input_ctx->endpointContext[0].TRDequeuePointerLo |= 1;
    input_ctx->endpointContext[0].TRDequeuePointerHi = ep0_phys >> 32;

    xHCI::TRB::Any cmd;
    cmd.data[0] = 0; cmd.data[1] = 0; cmd.data[2] = 0; cmd.data[3] = 0;
    std::uint64_t input_ctx_phys = reinterpret_cast<std::uintptr_t>(input_ctx) - oz::DIRECT_MAP_OFFSET;
    cmd.data[0] = input_ctx_phys & 0xFFFFFFFF;
    cmd.data[1] = input_ctx_phys >> 32;
    cmd.data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::AddressDeviceCommand) << 10) | (slot_id << 24);
    
    devices_[slot_id].input_context = input_ctx;
    devices_[slot_id].state = DeviceState::Addressed;
    command_ring_.push(cmd);
    ringDoorbell(0);
}

void Controller::issueGetDescriptor(std::uint8_t slot_id, std::uint8_t desc_type, std::uint8_t desc_index, std::uint16_t length) {
    if (!fm_) return;
    
    if (!devices_[slot_id].control_buffer) {
        oz::PageBlock<> data_block = fm_->allocateBlock(0);
        devices_[slot_id].control_buffer = oz::phys_to_virt(fm_->getPhysicalAddress(data_block));
    }
    
    std::uint64_t data_phys = reinterpret_cast<std::uintptr_t>(devices_[slot_id].control_buffer) - oz::DIRECT_MAP_OFFSET;
    
    xHCI::TRB::Any setup;
    setup.data[0] = 0; setup.data[1] = 0; setup.data[2] = 0; setup.data[3] = 0;
    USB::SetupData setup_data;
    setup_data.request_type = 0x80;
    setup_data.request = static_cast<std::uint8_t>(USB::StandardRequest::GetDescriptor);
    setup_data.value = (desc_type << 8) | desc_index;
    setup_data.index = 0;
    setup_data.length = length;
    
    std::uint32_t* setup_data_ptr = reinterpret_cast<std::uint32_t*>(&setup_data);
    setup.data[0] = setup_data_ptr[0];
    setup.data[1] = setup_data_ptr[1];
    setup.data[2] = 8;
    setup.data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::SetupStage) << 10) | (3 << 16) | (1 << 6);

    devices_[slot_id].transfer_rings[0].push(setup);

    xHCI::TRB::Any data;
    data.data[0] = 0; data.data[1] = 0; data.data[2] = 0; data.data[3] = 0;
    data.data[0] = data_phys & 0xFFFFFFFF;
    data.data[1] = data_phys >> 32;
    data.data[2] = length;
    data.data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::DataStage) << 10) | (1 << 16);

    devices_[slot_id].transfer_rings[0].push(data);

    xHCI::TRB::Any status;
    status.data[0] = 0; status.data[1] = 0; status.data[2] = 0; status.data[3] = 0;
    status.data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::StatusStage) << 10) | (1 << 5);

    devices_[slot_id].transfer_rings[0].push(status);
    
    ringDoorbell(slot_id, 1);
}

void Controller::parseConfigurationDescriptor(std::uint8_t slot_id) {
    std::uint8_t* buf = reinterpret_cast<std::uint8_t*>(devices_[slot_id].control_buffer);
    USB::ConfigurationDescriptor* conf_desc = reinterpret_cast<USB::ConfigurationDescriptor*>(buf);
    
    devices_[slot_id].config_value = conf_desc->configuration_value;
    
    std::uint16_t target_max_packet_size = 0;
    std::uint8_t target_interval = 0;
    std::uint8_t target_ep_addr = keyboard_driver_.parseConfiguration(conf_desc, target_max_packet_size, target_interval);
    
    if (target_ep_addr != 0) {
        issueConfigureEndpointCommand(slot_id, target_ep_addr, target_max_packet_size, target_interval);
    }
}

void Controller::issueConfigureEndpointCommand(std::uint8_t slot_id, std::uint8_t endpoint_address, std::uint16_t max_packet_size, std::uint8_t interval) {
    xHCI::InputContext* input_ctx = devices_[slot_id].input_context;
    
    for(std::size_t i = 0; i < sizeof(xHCI::InputContext); ++i) {
        if (i < 8) continue;
        reinterpret_cast<std::uint8_t*>(input_ctx)[i] = 0;
    }
    
    std::uint8_t ep_num = endpoint_address & 0x0F;
    std::uint8_t dir_in = (endpoint_address & 0x80) ? 1 : 0;
    std::uint8_t dci = ep_num * 2 + dir_in;
    
    input_ctx->inputControlContext.DropContextFlags = 0;
    input_ctx->inputControlContext.AddContextFlags = (1 << dci) | (1 << 0);
    
    std::uint64_t dev_ctx_phys = dcbaa_[slot_id];
    xHCI::DeviceContext* dev_ctx = reinterpret_cast<xHCI::DeviceContext*>(dev_ctx_phys + oz::DIRECT_MAP_OFFSET);
    input_ctx->slotContext = dev_ctx->slotContext;
    
    std::uint32_t current_entries = input_ctx->slotContext.Offset00h >> 27;
    if (dci > current_entries) {
        input_ctx->slotContext.Offset00h &= ~(0x1F << 27);
        input_ctx->slotContext.Offset00h |= (dci << 27);
    }
    
    oz::PageBlock<> ep_ring_block = fm_->allocateBlock(0);
    xHCI::TRB::Any* ep_ring_buf = reinterpret_cast<xHCI::TRB::Any*>(oz::phys_to_virt(fm_->getPhysicalAddress(ep_ring_block)));
    std::size_t ep_ring_size = 4096 / sizeof(xHCI::TRB::Any);
    
    for (std::size_t i = 0; i < ep_ring_size; ++i) {
        ep_ring_buf[i].data[0] = 0; ep_ring_buf[i].data[1] = 0;
        ep_ring_buf[i].data[2] = 0; ep_ring_buf[i].data[3] = 0;
    }
    
    devices_[slot_id].transfer_rings[dci - 1].initialize(ep_ring_buf, ep_ring_size);
    
    ep_ring_buf[ep_ring_size - 1].data[0] = static_cast<std::uint32_t>(fm_->getPhysicalAddress(ep_ring_block).get());
    ep_ring_buf[ep_ring_size - 1].data[1] = static_cast<std::uint32_t>(fm_->getPhysicalAddress(ep_ring_block).get() >> 32);
    ep_ring_buf[ep_ring_size - 1].data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::Link) << 10) | 2;
    
    input_ctx->endpointContext[dci - 1].EPState = 0;
    input_ctx->endpointContext[dci - 1].Offset01h = 0;
    input_ctx->endpointContext[dci - 1].Offset04h = (7 << 3) | (3 << 1); 
    input_ctx->endpointContext[dci - 1].MaxPacketSize = max_packet_size;
    input_ctx->endpointContext[dci - 1].Interval = interval;
    input_ctx->endpointContext[dci - 1].AverageTRBLength = 8;
    
    std::uint64_t ep_phys = fm_->getPhysicalAddress(ep_ring_block).get();
    input_ctx->endpointContext[dci - 1].TRDequeuePointerLo = ep_phys & 0xFFFFFFFF;
    input_ctx->endpointContext[dci - 1].TRDequeuePointerLo |= 1;
    input_ctx->endpointContext[dci - 1].TRDequeuePointerHi = ep_phys >> 32;
    
    xHCI::TRB::Any cmd;
    cmd.data[0] = 0; cmd.data[1] = 0; cmd.data[2] = 0; cmd.data[3] = 0;
    std::uint64_t input_ctx_phys = reinterpret_cast<std::uintptr_t>(input_ctx) - oz::DIRECT_MAP_OFFSET;
    cmd.data[0] = input_ctx_phys & 0xFFFFFFFF;
    cmd.data[1] = input_ctx_phys >> 32;
    cmd.data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::ConfigureEndpointCommand) << 10) | (slot_id << 24);
    
    devices_[slot_id].dci_interrupt_in = dci;
    devices_[slot_id].state = DeviceState::ConfiguringEndpoint;
    command_ring_.push(cmd);
    ringDoorbell(0);
}

void Controller::issueKeyboardTransfer(std::uint8_t slot_id) {
    if (!fm_) return;
    
    if (!devices_[slot_id].report_buffer) {
        oz::PageBlock<> report_block = fm_->allocateBlock(0);
        devices_[slot_id].report_buffer = reinterpret_cast<std::uint8_t*>(oz::phys_to_virt(fm_->getPhysicalAddress(report_block)));
    }
    
    std::uint64_t report_phys = reinterpret_cast<std::uintptr_t>(devices_[slot_id].report_buffer) - oz::DIRECT_MAP_OFFSET;
    
    xHCI::TRB::Any normal;
    normal.data[0] = 0; normal.data[1] = 0; normal.data[2] = 0; normal.data[3] = 0;
    normal.data[0] = report_phys & 0xFFFFFFFF;
    normal.data[1] = report_phys >> 32;
    normal.data[2] = 8;
    normal.data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::Normal) << 10) | (1 << 5);
    
    std::uint8_t dci = devices_[slot_id].dci_interrupt_in;
    devices_[slot_id].transfer_rings[dci - 1].push(normal);
    ringDoorbell(slot_id, dci);
}

void Controller::issueSetConfiguration(std::uint8_t slot_id) {
    if (!fm_) return;
    
    xHCI::TRB::Any setup;
    setup.data[0] = 0; setup.data[1] = 0; setup.data[2] = 0; setup.data[3] = 0;
    USB::SetupData setup_data;
    setup_data.request_type = 0x00;
    setup_data.request = static_cast<std::uint8_t>(USB::StandardRequest::SetConfiguration);
    setup_data.value = devices_[slot_id].config_value;
    setup_data.index = 0;
    setup_data.length = 0;
    
    std::uint32_t* setup_data_ptr = reinterpret_cast<std::uint32_t*>(&setup_data);
    setup.data[0] = setup_data_ptr[0];
    setup.data[1] = setup_data_ptr[1];
    setup.data[2] = 8;
    setup.data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::SetupStage) << 10) | (0 << 16) | (1 << 6);

    devices_[slot_id].transfer_rings[0].push(setup);

    xHCI::TRB::Any status;
    status.data[0] = 0; status.data[1] = 0; status.data[2] = 0; status.data[3] = 0;
    status.data[3] = (static_cast<std::uint32_t>(xHCI::TRB::Type::StatusStage) << 10) | (1 << 5) | (1 << 16);

    devices_[slot_id].transfer_rings[0].push(status);
    
    devices_[slot_id].state = DeviceState::SettingConfiguration;
    
    ringDoorbell(slot_id, 1);
}

} // namespace xHCIUtils
