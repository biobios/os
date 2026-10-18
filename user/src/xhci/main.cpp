#include "syscall.hpp"
#include "xHCIDriver.hpp"
#include "HIDKeyboard.hpp"
#include "../common/ipc/Protocol.hpp"

static constexpr std::uint32_t CAP_CONSOLE_WRITE = 1;
static constexpr std::uint32_t CAP_IRQ_RECV = 2;
static constexpr std::uint32_t CAP_IRQ_HANDLER = 3;

static HID::Keyboard g_keyboard;

static void onHIDReport(const std::uint8_t* report) {
    g_keyboard.processReport(report);
    HID::KeyEvent event;
    while (g_keyboard.pop(event)) {
        if (event.state == HID::KeyState::Pressed) {
            oz::IPCMessage msg{};
            msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMEventType::KEY_DOWN);
            msg.data[1] = static_cast<std::uint64_t>(event.ascii);
            msg.data[2] = static_cast<std::uint64_t>(event.keycode);

            std::uint32_t mod = 0;
            if (event.modifiers.left_shift || event.modifiers.right_shift) mod |= static_cast<std::uint32_t>(oz::ipc::WMModifier::SHIFT);
            if (event.modifiers.left_ctrl || event.modifiers.right_ctrl)   mod |= static_cast<std::uint32_t>(oz::ipc::WMModifier::CTRL);
            if (event.modifiers.left_alt || event.modifiers.right_alt)     mod |= static_cast<std::uint32_t>(oz::ipc::WMModifier::ALT);
            if (event.modifiers.left_gui || event.modifiers.right_gui)     mod |= static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER);
            msg.data[3] = static_cast<std::uint64_t>(mod);

            oz::sys_channel_send(CAP_CONSOLE_WRITE, &msg);
        }
    }
}

// Global storage for the Controller to avoid stack overflow
// (Controller is ~290KB due to devices_[256], stack is only 8KB)
static xHCIDriver::Controller* g_xhci = nullptr;

extern "C" void _start() {
    oz::sys_debug_print("xHCI Server started!\n\r");
    
    // Map xHCI MMIO from Frame cap slot 241
    static constexpr std::uint64_t MMIO_VIRT = 0x200000000000;
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, 241, MMIO_VIRT, 0x10 | 0x01, 16 * 4096) != 0) {
        oz::sys_debug_print("Failed to map xHCI MMIO!\n\r");
        oz::sys_exit(1);
    }
    
    // Heap allocation to avoid stack overflow
    g_xhci = new xHCIDriver::Controller(MMIO_VIRT);
    g_xhci->setReportCallback(onHIDReport);
    
    if (!g_xhci->initialize()) {
        oz::sys_debug_print("xHCI initialization failed!\n\r");
        oz::sys_exit(1);
    }
    
    g_xhci->scanInitialPorts();
    g_xhci->processEvents(); // Process initial port status changes
    
    oz::sys_debug_print("xHCI ready, waiting for keyboard events...\n\r");
    
    // Main event loop: wait for IRQ, process events
    while (true) {
        oz::IPCMessage irq_msg{};
        oz::sys_channel_recv(CAP_IRQ_RECV, &irq_msg); // Block until IRQ fires
        
        g_xhci->processEvents();
        g_xhci->clearEventInterruptStatus();
        
        oz::sys_irq_ack(CAP_IRQ_HANDLER); // Acknowledge interrupt
    }
}

