#include "syscall.hpp"
#include "xHCIDriver.hpp"
#include "HIDKeyboard.hpp"

static constexpr std::uint32_t CAP_CONSOLE_WRITE = 1;
static constexpr std::uint32_t CAP_IRQ_RECV = 2;
static constexpr std::uint32_t CAP_IRQ_HANDLER = 3;

static HID::Keyboard g_keyboard;

static void onHIDReport(const std::uint8_t* report) {
    g_keyboard.processReport(report);
    HID::KeyEvent event;
    while (g_keyboard.pop(event)) {
        if (event.state == HID::KeyState::Pressed && event.ascii != 0) {
            // Send to console
            oz::IPCMessage msg{};
            char buf[32] = {};
            buf[0] = event.ascii;
            for (int k = 0; k < 4; k++)
                msg.data[k] = *reinterpret_cast<std::uint64_t*>(&buf[k*8]);
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

