#include "syscall.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"
#include "../common/utils/BootInfo.hpp"
#include "../common/utils/utils.hpp"

// DrivMan (Device Manager): Broker for hardware capabilities
OZ_MANIFEST((oz::ProcessManifest{
    .magic = oz::ProcessManifest::MAGIC,
    .version = 1,
    .provides = 0,
    .required = static_cast<std::uint32_t>(oz::ServiceId::CONSOLE),
    .device_needed = oz::DeviceRequirement::NONE,
    .needs_untyped = true,
    .default_data_dir = {}
}));

static constexpr std::uint32_t CAP_CONSOLE_WRITE = 1;
static constexpr std::uint32_t CAP_DRIVMAN_RECV  = 2;
static constexpr std::uint32_t TEMP_XFER_SLOT    = 50;

static constexpr std::uint64_t BOOTINFO_VADDR    = 0x600000000000ULL;

static bool match_name(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a++ != *b++) return false;
    }
    return (*a == *b);
}

static void to_hex_str(std::uint64_t val, char* out) {
    const char* digits = "0123456789ABCDEF";
    for (int i = 15; i >= 0; --i) {
        out[i] = digits[val & 0xF];
        val >>= 4;
    }
    out[16] = '\0';
}

extern "C" void _start() {
    oz::sys_write("[DrivMan] Device Manager server started (Ring 3 Broker)!\n\r");

    const volatile oz::BootInfo* boot_info = nullptr;

    // Map BootInfo Frame (slot 243) if present
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_BOOTINFO, BOOTINFO_VADDR, 0x00, 4096) == 0) {
        boot_info = reinterpret_cast<const volatile oz::BootInfo*>(BOOTINFO_VADDR);
        oz::sys_write("[DrivMan] BootInfo mapped: w=");
        char numbuf[17];
        to_hex_str(boot_info->fb_width, numbuf);
        oz::sys_write(numbuf);
        oz::sys_write(" h=");
        to_hex_str(boot_info->fb_height, numbuf);
        oz::sys_write(numbuf);
        oz::sys_write(" sz=");
        to_hex_str(boot_info->fb_size, numbuf);
        oz::sys_write(numbuf);
        oz::sys_write("\n\r");
    }

    oz::ReplyToken token = oz::INVALID_REPLY_TOKEN;
    oz::IPCMessage resp{};
    oz::IPCMessage req{};

    while (true) {
        token = oz::sys_reply_recv(token, CAP_DRIVMAN_RECV, (token != oz::INVALID_REPLY_TOKEN) ? &resp : nullptr, &req);
        if (token == oz::INVALID_REPLY_TOKEN) {
            continue;
        }

        auto cmd = static_cast<oz::ipc::DrivCmd>(req.data[0]);
        resp = oz::IPCMessage{};

        if (cmd == oz::ipc::DrivCmd::GET_DEVICE_CAP) {
            char dev_name[25] = {};
            const char* src = reinterpret_cast<const char*>(&req.data[1]);
            for (int i = 0; i < 24 && src[i] != '\0'; ++i) {
                dev_name[i] = src[i];
            }

            if (match_name(dev_name, "ahci")) {
                if (oz::sys_cap_copy(TEMP_XFER_SLOT, oz::CAP_AHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL)) == 0) {
                    resp.data[0] = 0; // Success
                    resp.data[1] = boot_info ? boot_info->ahci_abar_phys : 0;
                    resp.has_cap = true;
                    resp.transferred_cap = TEMP_XFER_SLOT;
                    oz::sys_write("[DrivMan] Brokered AHCI MMIO capability to driver.\n\r");
                } else {
                    resp.data[0] = 1; // Not available
                }
            } else if (match_name(dev_name, "xhci")) {
                if (oz::sys_cap_copy(TEMP_XFER_SLOT, oz::CAP_XHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL)) == 0) {
                    resp.data[0] = 0; // Success
                    resp.data[1] = boot_info ? boot_info->xhci_bar_phys : 0;
                    resp.has_cap = true;
                    resp.transferred_cap = TEMP_XFER_SLOT;
                    oz::sys_write("[DrivMan] Brokered xHCI MMIO capability to driver.\n\r");
                } else {
                    resp.data[0] = 1;
                }
            } else if (match_name(dev_name, "xhci_irq")) {
                if (req.has_cap && req.transferred_cap != oz::INVALID_CAP_ID) {
                    std::uint32_t irq_handler = oz::sys_irq_handler_create(oz::CAP_UNTYPED, 0x40, req.transferred_cap);
                    if (irq_handler != oz::INVALID_CAP_ID) {
                        resp.data[0] = 0;
                        resp.has_cap = true;
                        resp.transferred_cap = irq_handler;
                        oz::sys_write("[DrivMan] Brokered xHCI IRQ handler capability to driver.\n\r");
                    } else {
                        resp.data[0] = 2; // Failed to create IRQ handler
                    }
                } else {
                    resp.data[0] = 1; // Missing IRQ channel cap
                }
            } else if (match_name(dev_name, "fb")) {
                if (oz::sys_cap_copy(TEMP_XFER_SLOT, oz::CAP_FRAMEBUFFER, static_cast<std::uint32_t>(oz::CapRights::ALL)) == 0) {
                    resp.data[0] = 0;
                    std::uint64_t w = boot_info ? boot_info->fb_width : 0;
                    std::uint64_t h = boot_info ? boot_info->fb_height : 0;
                    resp.data[1] = (w << 32) | h;
                    resp.data[2] = boot_info ? boot_info->fb_size : 0;
                    resp.data[3] = boot_info ? boot_info->fb_phys_addr : 0;
                    resp.has_cap = true;
                    resp.transferred_cap = TEMP_XFER_SLOT;
                    oz::sys_write("[DrivMan] Brokered FrameBuffer capability to display server.\n\r");
                    oz::sys_write("[DrivMan] fb resp: data1=");
                    char dbuf[17];
                    to_hex_str(resp.data[1], dbuf);
                    oz::sys_write(dbuf);
                    oz::sys_write(" data2=");
                    to_hex_str(resp.data[2], dbuf);
                    oz::sys_write(dbuf);
                    oz::sys_write("\n\r");
                } else {
                    resp.data[0] = 1;
                }
            } else {
                resp.data[0] = 0xFE; // Unknown device
            }
        } else if (cmd == oz::ipc::DrivCmd::GET_BOOT_INFO) {
            if (oz::sys_cap_copy(TEMP_XFER_SLOT, oz::CAP_BOOTINFO, static_cast<std::uint32_t>(oz::CapRights::ALL)) == 0) {
                resp.data[0] = 0;
                resp.has_cap = true;
                resp.transferred_cap = TEMP_XFER_SLOT;
            } else {
                resp.data[0] = 1;
            }
        } else {
            resp.data[0] = 0xFF; // Unknown command
        }
    }
}
