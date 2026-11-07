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
static std::uint32_t g_irq_handler_cap = 0;
static std::uint32_t g_irq_baton_a = 0;
static std::uint32_t g_irq_baton_b = 0;

static void xhci_irq_worker() {
    oz::sys_debug_print("[xHCI Worker] IRQ Worker Thread active, entering sys_irq_wait loop...\n\r");
    std::uint32_t current_baton = g_irq_baton_a;
    std::uint32_t next_baton = g_irq_baton_b;

    // Initial wait reservation
    oz::sys_irq_wait(g_irq_handler_cap, current_baton);

    while (true) {
        oz::sys_debug_print("[xHCI Worker] irq_wait woke up\n\r");

        if (g_xhci) {
            g_xhci->processEvents();
            g_xhci->clearEventInterruptStatus();
        }

        // Ping-Pong rotation: register next_baton and yield current_baton
        oz::sys_irq_wait(g_irq_handler_cap, next_baton);
        std::uint32_t tmp = current_baton;
        current_baton = next_baton;
        next_baton = tmp;
    }
}

extern "C" void oz_main(const oz::ProcessStartupInfo* info) {
    oz::LocalNamespace::init(info);
    oz::sys_debug_print("xHCI Server started (Normal Thread)!\n\r");

    // Map shared Keyboard Ring Buffer Frame (slot 4)
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, CAP_KBD_RING_FRAME, KBD_RING_VIRT, 0x01, 4096) == 0) {
        g_kbd_ring = reinterpret_cast<volatile oz::WMInputRing*>(KBD_RING_VIRT);
        oz::sys_debug_print("[xHCI] Shared Keyboard Ring Buffer mapped successfully.\n\r");
    }

    std::uint32_t mmio_cap = oz::INVALID_CAP_ID;
    std::uint32_t drivman_cap = oz::LocalNamespace::get_cap(oz::CapRole::DRIVMAN);

    if (drivman_cap != 0) {
        // Phase 2 Supervisor Protocol
        oz::IPCMessage sess_req{};
        sess_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::INIT_DEVICE_SESSION);
        oz::IPCMessage sess_resp{};
        if (oz::sys_call(drivman_cap, &sess_req, &sess_resp) && sess_resp.data[0] == 0) {
            oz::sys_debug_print("[xHCI] Initialized device session with DrivMan.\n\r");

            // 1. Request MMIO resource (res_type = 1, res_idx = 0)
            oz::IPCMessage res_req{};
            res_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::GET_DEVICE_RESOURCE);
            res_req.data[1] = 1; // MMIO_BAR
            res_req.data[2] = 0; // BAR 0
            oz::IPCMessage res_resp{};
            if (oz::sys_call_recv_cap(drivman_cap, &res_req, &res_resp) && res_resp.data[0] == 0 && res_resp.has_cap) {
                mmio_cap = res_resp.transferred_cap;
                oz::sys_debug_print("[xHCI] Obtained MMIO frame cap from DrivMan.\n\r");
            } else {
                oz::sys_debug_print("[xHCI] Failed to obtain MMIO frame cap from DrivMan!\n\r");
                oz::sys_exit(1);
            }

            // 2. Request IRQ Handler resource (res_type = 2)
            std::uint32_t irq_send = 0, irq_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &irq_send, &irq_recv);

            oz::IPCMessage irq_req{};
            irq_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::GET_DEVICE_RESOURCE);
            irq_req.data[1] = 2; // IRQ_HANDLER
            irq_req.has_cap = true;
            irq_req.transferred_cap = irq_send;
            oz::IPCMessage irq_resp{};
            if (oz::sys_call_recv_cap(drivman_cap, &irq_req, &irq_resp) && irq_resp.data[0] == 0 && irq_resp.has_cap) {
                g_irq_handler_cap = irq_resp.transferred_cap;
                oz::sys_debug_print("[xHCI] Obtained IRQ Handler cap from DrivMan.\n\r");
            } else {
                oz::sys_debug_print("[xHCI] Failed to obtain IRQ Handler cap from DrivMan!\n\r");
                oz::sys_exit(1);
            }
        } else {
            oz::sys_debug_print("[xHCI] Failed to initialize device session with DrivMan!\n\r");
            oz::sys_exit(1);
        }
    } else {
        // Fallback for standalone test harness (xhcitest)
        mmio_cap = 241;
        g_irq_handler_cap = CAP_IRQ_HANDLER; // slot 3
        oz::sys_debug_print("[xHCI] Using legacy/standalone pre-wired caps (slot 241 & 3).\n\r");
    }

    // Map xHCI MMIO
    static constexpr std::uint64_t MMIO_VIRT = 0x200000000000;
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, mmio_cap, MMIO_VIRT, 0x10 | 0x01, 16 * 4096) != 0) {
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

    // Spawn dedicated Worker Thread (normal TCB) for hardware IRQ processing
    static constexpr std::uint64_t IRQ_STACK_TOP = 0x700020000000ULL;
    std::uint32_t irq_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 4); // 16KB stack
    if (irq_stack == oz::INVALID_CAP_ID ||
        oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, irq_stack, IRQ_STACK_TOP - 16384, 0x03, 16384) != 0) {
        oz::sys_debug_print("[xHCI] Failed to allocate IRQ Worker stack!\n\r");
        oz::sys_exit(1);
    }

    std::uint32_t ithread = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    if (ithread == oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[xHCI] Failed to create TCB object!\n\r");
        oz::sys_exit(1);
    }

    if (oz::sys_tcb_configure(ithread, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                              reinterpret_cast<std::uint64_t>(xhci_irq_worker), IRQ_STACK_TOP - 8) != 0) {
        oz::sys_debug_print("[xHCI] Failed to configure TCB!\n\r");
        oz::sys_exit(1);
    }

    std::uint32_t own_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_tcb_bind_baton(ithread, own_baton);

    // Create Ping-Pong IRQ Batons (100ms quantum budget)
    static constexpr std::uint64_t IRQ_BUDGET_US = 100000;
    g_irq_baton_a = oz::sys_baton_create_irq(oz::CAP_IRQ_CONTROL, oz::CAP_UNTYPED, IRQ_BUDGET_US);
    g_irq_baton_b = oz::sys_baton_create_irq(oz::CAP_IRQ_CONTROL, oz::CAP_UNTYPED, IRQ_BUDGET_US);
    if (g_irq_baton_a == oz::INVALID_CAP_ID || g_irq_baton_b == oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[xHCI] Failed to create IRQ batons!\n\r");
        oz::sys_exit(1);
    }

    std::uint32_t inotif = oz::sys_notification_create(oz::CAP_UNTYPED);
    static constexpr std::uint64_t BADGE_IRQ_REPLENISH = 1ULL << 12;
    oz::sys_thread_set_notification(ithread, inotif, BADGE_IRQ_REPLENISH);

    // Initial hand-off on own_baton to enter sys_irq_wait reservation
    oz::sys_baton_inject(own_baton, 100000);
    oz::sys_baton_switch(own_baton);

    // Notify DrivMan that device is online
    if (drivman_cap != 0) {
        oz::IPCMessage on_msg{};
        on_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::NOTIFY_DEVICE_ONLINE);
        oz::IPCMessage on_resp{};
        oz::sys_call(drivman_cap, &on_msg, &on_resp);
    }

    oz::sys_debug_print("xHCI ready, waiting for events (Supervisor Loop active)...\n\r");

    // Main Thread Supervisor Loop: Replenish IRQ thread budget when needed
    while (true) {
        std::uint64_t badge = 0;
        int wait_res = oz::sys_notification_wait(inotif, &badge);
        if (wait_res == 0 && (badge & BADGE_IRQ_REPLENISH)) {
            oz::sys_debug_print("[xHCI Main] IRQ Thread budget replenishment / resume\n\r");
            oz::sys_baton_set_budget(g_irq_baton_a, IRQ_BUDGET_US);
            oz::sys_baton_set_budget(g_irq_baton_b, IRQ_BUDGET_US);
            oz::sys_baton_inject(own_baton, 100000);
            oz::sys_baton_switch(own_baton);
        }
    }
}

