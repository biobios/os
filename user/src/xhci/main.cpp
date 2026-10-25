#include "syscall.hpp"
#include "xHCIDriver.hpp"
#include "HIDKeyboard.hpp"
#include "ring_buffer.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"

OZ_MANIFEST((oz::ProcessManifest{
    .magic = oz::ProcessManifest::MAGIC,
    .version = 1,
    .provides = 0,
    .required = static_cast<std::uint32_t>(oz::ServiceId::WM),
    .device_needed = oz::DeviceRequirement::XHCI,
    .needs_untyped = true,
    .default_data_dir = {}
}));

static constexpr std::uint32_t CAP_CONSOLE_WRITE  = 1;
static constexpr std::uint32_t CAP_IRQ_RECV       = 2;
static constexpr std::uint32_t CAP_IRQ_HANDLER    = 3;
static constexpr std::uint32_t CAP_KBD_RING_FRAME = 4;
static constexpr std::uint32_t CAP_NOTIF_WM       = 5;
static constexpr std::uint64_t KBD_RING_VIRT      = 0x210000000000ULL;
static constexpr std::uint64_t BADGE_KEYBOARD     = 1ULL << 1;

static HID::Keyboard g_keyboard;
static volatile oz::WMInputRing* g_kbd_ring = nullptr;

static void onHIDReport(const std::uint8_t* report) {
    g_keyboard.processReport(report);
    HID::KeyEvent event;
    while (g_keyboard.pop(event)) {
        if (event.state == HID::KeyState::Pressed) {
            oz::sys_debug_print("[xHCI] Key Pressed: ascii='");
            char cbuf[2] = {event.ascii ? event.ascii : '?', '\0'};
            oz::sys_debug_print(cbuf);
            oz::sys_debug_print("'\n\r");

            std::uint32_t mod = 0;
            if (event.modifiers.left_shift || event.modifiers.right_shift) mod |= static_cast<std::uint32_t>(oz::ipc::WMModifier::SHIFT);
            if (event.modifiers.left_ctrl || event.modifiers.right_ctrl)   mod |= static_cast<std::uint32_t>(oz::ipc::WMModifier::CTRL);
            if (event.modifiers.left_alt || event.modifiers.right_alt)     mod |= static_cast<std::uint32_t>(oz::ipc::WMModifier::ALT);
            if (event.modifiers.left_gui || event.modifiers.right_gui)     mod |= static_cast<std::uint32_t>(oz::ipc::WMModifier::SUPER);

            if (g_kbd_ring) {
                oz::WMInputEvent input_ev{};
                input_ev.type = static_cast<std::uint32_t>(oz::ipc::WMEventType::KEY_DOWN);
                input_ev.code = static_cast<std::uint32_t>(static_cast<std::uint8_t>(event.ascii)) |
                                (static_cast<std::uint32_t>(event.keycode) << 8);
                input_ev.modifiers = mod;

                bool pushed = g_kbd_ring->push(input_ev);
                oz::sys_debug_print("[xHCI] Pushed to kbd_ring: ");
                oz::sys_debug_print(pushed ? "OK, signaling WM\n\r" : "FULL!\n\r");
                oz::sys_notification_signal(CAP_NOTIF_WM, BADGE_KEYBOARD);
            } else {
                oz::IPCMessage msg{};
                msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMEventType::KEY_DOWN);
                msg.data[1] = static_cast<std::uint64_t>(event.ascii);
                msg.data[2] = static_cast<std::uint64_t>(event.keycode);
                msg.data[3] = static_cast<std::uint64_t>(mod);
                oz::IPCMessage resp{};
                oz::sys_call(CAP_CONSOLE_WRITE, &msg, &resp);
            }
        }
    }
}

// Global storage for the Controller to avoid stack overflow
// (Controller is ~290KB due to devices_[256], stack is only 8KB)
static xHCIDriver::Controller* g_xhci = nullptr;

OZ_PROCESS_ENTRY() {
    oz::LocalNamespace::init(info);
    oz::sys_debug_print("xHCI Server started!\n\r");

    // Map shared Keyboard Ring Buffer Frame (slot 4)
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, CAP_KBD_RING_FRAME, KBD_RING_VIRT, 0x01, 4096) == 0) {
        g_kbd_ring = reinterpret_cast<volatile oz::WMInputRing*>(KBD_RING_VIRT);
        oz::sys_debug_print("[xHCI] Shared Keyboard Ring Buffer mapped successfully.\n\r");
    }
    
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
    
    // Main event loop: wait for IRQ via v2 IRQ Baton (Clean Preemption), process events
    while (true) {
        oz::sys_irq_wait(CAP_IRQ_HANDLER);
        oz::sys_debug_print("[xHCI] irq_wait woke up\n\r");
        
        g_xhci->processEvents();
        g_xhci->clearEventInterruptStatus();
    }
}

