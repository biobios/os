#include "syscall.hpp"
#include "loader.hpp"
#include "../common/test_harness.hpp"
#include "../../src/common/ipc/Protocol.hpp"
#include "../../src/common/utils/BootInfo.hpp"

namespace oz::xhcitest {

static constexpr std::uint64_t BOOTINFO_VADDR = 0x600000000000ULL;
static constexpr std::uint64_t INITRD_VADDR   = 0x500000000000ULL;

} // namespace oz::xhcitest

extern "C" void _start() {
    oz::sys_debug_print("\n\r========================================================\n\r");
    oz::sys_debug_print(" [OZ_OS] xHCI & USB Keyboard Test Runner (xhcitest)\n\r");
    oz::sys_debug_print("========================================================\n\r");

    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_BOOTINFO, oz::xhcitest::BOOTINFO_VADDR, 0x00, 4096) != 0) {
        oz::test::report("XHCITEST", "Map BootInfo", false);
        oz::test::exit_qemu();
    }
    const volatile auto* boot_info = reinterpret_cast<const volatile oz::BootInfo*>(oz::xhcitest::BOOTINFO_VADDR);
    std::uint64_t initrd_size = boot_info->initrd_size;
    std::uint64_t xhci_bar_phys = boot_info->xhci_bar_phys;

    if (initrd_size > 0) {
        oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_INITRD, oz::xhcitest::INITRD_VADDR, 0x00, initrd_size);
    }
    const std::uint8_t* initrd_ptr = reinterpret_cast<const std::uint8_t*>(oz::xhcitest::INITRD_VADDR + boot_info->initrd_offset);

    TEST_REPORT("XHCITEST", "xHCI BAR physical address detected", xhci_bar_phys != 0);

    oz::loader::TarFile xhci_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "xhci");
    TEST_REPORT("XHCITEST", "Find xhci binary in initrd", xhci_file.data != nullptr);

    if (xhci_file.data && xhci_bar_phys != 0) {
        auto xhci_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, xhci_file.data, xhci_file.size, 0x550000000000ULL, nullptr, false);
        TEST_REPORT("XHCITEST", "Spawn xhci process", xhci_proc.success);

        if (xhci_proc.success) {
            std::uint32_t irq_send = 0;
            std::uint32_t irq_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &irq_send, &irq_recv);

            std::uint32_t key_send = 0, key_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &key_send, &key_recv);

            std::uint32_t irq_handler = oz::sys_irq_handler_create(oz::CAP_UNTYPED, 0x40, irq_send);
            std::uint32_t kbd_ring_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
            std::uint32_t notif_wm_kbd = oz::sys_notification_create(oz::CAP_UNTYPED);

            oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 1, key_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
            oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 2, irq_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
            oz::sys_cspace_cap_move(xhci_proc.cspace_cap, 3, irq_handler);
            oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 4, kbd_ring_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
            oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 5, notif_wm_kbd, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
            std::uint32_t xhci_ut = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory), 4 * 1024 * 1024);
            oz::sys_cspace_cap_move(xhci_proc.cspace_cap, oz::CAP_UNTYPED, xhci_ut);
            oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, oz::CAP_IRQ_CONTROL, oz::CAP_IRQ_CONTROL, static_cast<std::uint32_t>(oz::CapRights::ALL));
            oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 241, oz::CAP_XHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL));

            // Bind baton and dispatch xhci driver
            std::uint32_t xhci_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
            oz::sys_baton_inject(xhci_baton, 100000);
            oz::sys_tcb_bind_baton(xhci_proc.tcb_cap, xhci_baton);
            oz::sys_tcb_resume(xhci_proc.tcb_cap);

            oz::SchedResult res = oz::sys_baton_switch(xhci_baton);
            TEST_REPORT("XHCITEST", "xHCI driver initialization executed cleanly", res.reason != oz::SchedReason::TIMEOUT);
        }
    }

    oz::test::print_summary("XHCITEST");
    oz::test::exit_qemu();
}
