#include "syscall.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"
#include "../common/utils/BootInfo.hpp"
#include "../common/utils/utils.hpp"

// DrivMan (Device Manager): Supervisor for hardware capabilities & driver lifecycle
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
static constexpr std::uint32_t CAP_DRIVMAN_SEND  = 3;
static constexpr std::uint32_t CAP_PROCMAN_SEND  = 4;
static constexpr std::uint32_t CAP_VFS_SEND      = 6;
static constexpr std::uint32_t TEMP_XFER_SLOT    = 50;

static constexpr std::uint64_t BOOTINFO_VADDR    = 0x600000000000ULL;
static const volatile oz::BootInfo* g_boot_info   = nullptr;

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

static inline std::uint64_t get_tsc() {
    std::uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

// ===== PCI I/O Port Configuration Space Helpers =====
static inline std::uint32_t pci_config_read32(std::uint8_t bus, std::uint8_t dev, std::uint8_t func, std::uint8_t offset) {
    std::uint32_t address = (1U << 31)
                          | (static_cast<std::uint32_t>(bus) << 16)
                          | (static_cast<std::uint32_t>(dev) << 11)
                          | (static_cast<std::uint32_t>(func) << 8)
                          | (offset & 0xFC);
    oz::sys_ioport_write(oz::CAP_ROOT_IOPORT, 0xCF8, address, 4);
    return static_cast<std::uint32_t>(oz::sys_ioport_read(oz::CAP_ROOT_IOPORT, 0xCFC, 4));
}

static inline void pci_config_write32(std::uint8_t bus, std::uint8_t dev, std::uint8_t func, std::uint8_t offset, std::uint32_t val) {
    std::uint32_t address = (1U << 31)
                          | (static_cast<std::uint32_t>(bus) << 16)
                          | (static_cast<std::uint32_t>(dev) << 11)
                          | (static_cast<std::uint32_t>(func) << 8)
                          | (offset & 0xFC);
    oz::sys_ioport_write(oz::CAP_ROOT_IOPORT, 0xCF8, address, 4);
    oz::sys_ioport_write(oz::CAP_ROOT_IOPORT, 0xCFC, val, 4);
}

static inline std::uint16_t pci_config_read16(std::uint8_t bus, std::uint8_t dev, std::uint8_t func, std::uint8_t offset) {
    std::uint32_t val32 = pci_config_read32(bus, dev, func, offset);
    return static_cast<std::uint16_t>((val32 >> ((offset & 2) * 8)) & 0xFFFF);
}

static inline void pci_config_write16(std::uint8_t bus, std::uint8_t dev, std::uint8_t func, std::uint8_t offset, std::uint16_t val) {
    std::uint32_t current = pci_config_read32(bus, dev, func, offset);
    std::uint32_t shift = (offset & 2) * 8;
    std::uint32_t mask = 0xFFFF << shift;
    std::uint32_t new_val = (current & ~mask) | (static_cast<std::uint32_t>(val) << shift);
    pci_config_write32(bus, dev, func, offset, new_val);
}

// ===== Device Supervisor State & Radix-64 Notification Tree =====
enum class DeviceState : std::uint8_t {
    FREE = 0,
    DETECTED,
    STARTING,
    ONLINE,
    FAULTED,
    QUARANTINED,
    FAILED,
    REMOVED
};

struct DeviceEntry {
    std::uint32_t dev_id{0};
    DeviceState   state{DeviceState::FREE};

    std::uint8_t  bus{0};
    std::uint8_t  dev{0};
    std::uint8_t  func{0};
    std::uint16_t vendor_id{0};
    std::uint16_t device_id{0};
    std::uint8_t  class_code{0};
    std::uint8_t  subclass{0};
    std::uint8_t  prog_if{0};

    std::uint64_t mmio_phys[6]{};
    std::uint64_t mmio_size[6]{};
    std::uint32_t mmio_caps[6]{};
    std::uint8_t  num_bars{0};
    std::uint8_t  irq_type{0}; // 0 = none/INTx, 1 = MSI, 2 = MSI-X
    std::uint8_t  irq_line{0};

    std::uint16_t instance_id{0};
    const char*   driver_binary{nullptr};

    std::uint32_t pid{0};
    std::uint32_t exit_notif_cap{oz::INVALID_CAP_ID};
    std::uint32_t irq_handler_cap{oz::INVALID_CAP_ID};
    std::uint64_t reconnect_token{0};

    std::size_t   crash_count{0};
    std::uint64_t last_crash_tsc{0};
};

static constexpr std::size_t MAX_DEVICES = 64;
static constexpr std::size_t MAX_TREE_NODES = (MAX_DEVICES + 63) / 64;

static std::uint32_t g_root_notif = oz::INVALID_CAP_ID;
static std::uint32_t g_tree_nodes[MAX_TREE_NODES]{};
static DeviceEntry   g_devices[MAX_DEVICES]{};
static std::size_t   g_num_devices = 0;
static std::uint64_t g_token_seed = 0x9e3779b97f4a7c15ULL;

static DeviceEntry* find_device_by_id(std::uint32_t dev_id) {
    for (std::size_t i = 0; i < g_num_devices; ++i) {
        if (g_devices[i].dev_id == dev_id) return &g_devices[i];
    }
    return nullptr;
}

static const char* match_driver_rule(std::uint8_t c, std::uint8_t sc, std::uint8_t pi,
                                     std::uint16_t vid, std::uint16_t did) {
    constexpr std::size_t NUM_RULES = sizeof(oz::ipc::G_BINDING_RULES) / sizeof(oz::ipc::G_BINDING_RULES[0]);

    // Priority 1: Specific Match
    for (std::size_t i = 0; i < NUM_RULES; ++i) {
        const auto& r = oz::ipc::G_BINDING_RULES[i];
        if (r.vendor_id != 0xFFFF && r.device_id != 0xFFFF) {
            if (r.vendor_id == vid && r.device_id == did) {
                return r.driver_binary;
            }
        }
    }

    // Priority 2: Subsystem / Prog-IF Match
    for (std::size_t i = 0; i < NUM_RULES; ++i) {
        const auto& r = oz::ipc::G_BINDING_RULES[i];
        if (r.class_code == c && r.subclass == sc && r.prog_if == pi) {
            return r.driver_binary;
        }
    }

    // Priority 3: Class Match
    for (std::size_t i = 0; i < NUM_RULES; ++i) {
        const auto& r = oz::ipc::G_BINDING_RULES[i];
        if (r.class_code == c && r.subclass == sc) {
            return r.driver_binary;
        }
    }

    return nullptr;
}

static void scan_pci_bus(const volatile oz::BootInfo* boot_info) {
    oz::sys_write("[DrivMan] Scanning PCI bus for devices...\n\r");

    for (std::uint32_t bus = 0; bus < 256; ++bus) {
        for (std::uint32_t dev = 0; dev < 32; ++dev) {
            for (std::uint32_t func = 0; func < 8; ++func) {
                std::uint32_t id_reg = pci_config_read32(bus, dev, func, 0x00);
                std::uint16_t vendor_id = static_cast<std::uint16_t>(id_reg & 0xFFFF);
                std::uint16_t device_id = static_cast<std::uint16_t>(id_reg >> 16);

                if (vendor_id == 0xFFFF || vendor_id == 0x0000) {
                    if (func == 0) break;
                    continue;
                }

                std::uint32_t class_reg = pci_config_read32(bus, dev, func, 0x08);
                std::uint8_t prog_if = static_cast<std::uint8_t>((class_reg >> 8) & 0xFF);
                std::uint8_t subclass = static_cast<std::uint8_t>((class_reg >> 16) & 0xFF);
                std::uint8_t class_code = static_cast<std::uint8_t>((class_reg >> 24) & 0xFF);

                const char* binary = match_driver_rule(class_code, subclass, prog_if, vendor_id, device_id);
                if (binary && g_num_devices < MAX_DEVICES) {
                    std::uint32_t dev_id = static_cast<std::uint32_t>(g_num_devices + 1);
                    std::size_t entry_idx = g_num_devices++;
                    DeviceEntry& entry = g_devices[entry_idx];
                    entry.dev_id = dev_id;
                    entry.state = DeviceState::DETECTED;
                    entry.bus = static_cast<std::uint8_t>(bus);
                    entry.dev = static_cast<std::uint8_t>(dev);
                    entry.func = static_cast<std::uint8_t>(func);
                    entry.vendor_id = vendor_id;
                    entry.device_id = device_id;
                    entry.class_code = class_code;
                    entry.subclass = subclass;
                    entry.prog_if = prog_if;
                    entry.driver_binary = binary;

                    std::uint16_t inst = 0;
                    for (std::size_t k = 0; k < entry_idx; ++k) {
                        if (g_devices[k].driver_binary && match_name(g_devices[k].driver_binary, binary)) {
                            inst++;
                        }
                    }
                    entry.instance_id = inst;

                    std::uint8_t bar_count = 0;
                    for (std::uint8_t b = 0; b < 6; ++b) {
                        std::uint32_t bar_val = pci_config_read32(bus, dev, func, 0x10 + b * 4);
                        if (bar_val != 0 && bar_val != 0xFFFFFFFF) {
                            if ((bar_val & 0x01) == 0) {
                                std::uint64_t phys = bar_val & ~0xFULL;
                                bool is_64 = ((bar_val & 0x06) == 0x04);
                                if (is_64 && b + 1 < 6) {
                                    std::uint32_t bar_high = pci_config_read32(bus, dev, func, 0x10 + (b + 1) * 4);
                                    phys |= (static_cast<std::uint64_t>(bar_high) << 32);
                                    b++;
                                }
                                entry.mmio_phys[bar_count] = phys;
                                entry.mmio_size[bar_count] = 4096;
                                bar_count++;
                            }
                        }
                    }
                    entry.num_bars = bar_count;

                    if (match_name(binary, "/ahci")) {
                        entry.mmio_caps[0] = oz::CAP_AHCI_MMIO;
                        if (boot_info && boot_info->ahci_abar_phys != 0) {
                            entry.mmio_phys[0] = boot_info->ahci_abar_phys;
                        }
                    } else if (match_name(binary, "/xhci")) {
                        entry.mmio_caps[0] = oz::CAP_XHCI_MMIO;
                        if (boot_info && boot_info->xhci_bar_phys != 0) {
                            entry.mmio_phys[0] = boot_info->xhci_bar_phys;
                        }
                    } else if (match_name(binary, "/display")) {
                        entry.mmio_caps[0] = oz::CAP_FRAMEBUFFER;
                        if (boot_info && boot_info->fb_phys_addr != 0) {
                            entry.mmio_phys[0] = boot_info->fb_phys_addr;
                            entry.mmio_size[0] = boot_info->fb_size;
                        }
                    }

                    oz::sys_write("[DrivMan] Detected device: ");
                    oz::sys_write(binary);
                    oz::sys_write(" (dev_id=");
                    char num[16];
                    to_hex_str(dev_id, num);
                    oz::sys_write(&num[14]);
                    oz::sys_write(", inst=");
                    to_hex_str(inst, num);
                    oz::sys_write(&num[14]);
                    oz::sys_write(")\n\r");
                }

                if (func == 0) {
                    std::uint32_t hdr_reg = pci_config_read32(bus, dev, func, 0x0C);
                    if (((hdr_reg >> 16) & 0x80) == 0) {
                        break;
                    }
                }
            }
        }
    }

    if (boot_info && boot_info->fb_phys_addr != 0) {
        bool has_display = false;
        for (std::size_t i = 0; i < g_num_devices; ++i) {
            if (match_name(g_devices[i].driver_binary, "/display")) {
                has_display = true;
                break;
            }
        }
        if (!has_display && g_num_devices < MAX_DEVICES) {
            std::uint32_t dev_id = static_cast<std::uint32_t>(g_num_devices + 1);
            DeviceEntry& entry = g_devices[g_num_devices++];
            entry.dev_id = dev_id;
            entry.state = DeviceState::DETECTED;
            entry.driver_binary = "/display";
            entry.instance_id = 0;
            entry.num_bars = 1;
            entry.mmio_phys[0] = boot_info->fb_phys_addr;
            entry.mmio_size[0] = boot_info->fb_size;
            entry.mmio_caps[0] = oz::CAP_FRAMEBUFFER;
            oz::sys_write("[DrivMan] Detected FrameBuffer from BootInfo -> /display (dev_id=");
            char num[16];
            to_hex_str(dev_id, num);
            oz::sys_write(&num[14]);
            oz::sys_write(")\n\r");
        }
    }
}

static bool spawn_driver(DeviceEntry& dev) {
    if (CAP_PROCMAN_SEND == 0) return false;

    // Mint badged session cap
    std::uint32_t transfer_cap = oz::sys_cap_mint(CAP_DRIVMAN_SEND, static_cast<std::uint32_t>(oz::CapRights::ALL), dev.dev_id);
    if (transfer_cap == oz::INVALID_CAP_ID) {
        oz::sys_write("[DrivMan] ERROR: Failed to mint badged session cap!\n\r");
        dev.state = DeviceState::FAILED;
        return false;
    }

    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::SPAWN_DRIVER);
    char* dst = reinterpret_cast<char*>(&req.data[1]);
    const char* src = dev.driver_binary;
    int k = 0;
    for (; k < 15 && src[k] != '\0'; ++k) dst[k] = src[k];
    dst[k] = '\0';
    req.data[3] = dev.dev_id;
    if (transfer_cap != oz::INVALID_CAP_ID) {
        req.has_cap = true;
        req.transferred_cap = transfer_cap;
    }

    oz::IPCMessage resp{};
    if (oz::sys_call(CAP_PROCMAN_SEND, &req, &resp) && resp.data[0] == 0) {
        dev.pid = static_cast<std::uint32_t>(resp.data[1]);
        dev.exit_notif_cap = resp.has_cap ? resp.transferred_cap : oz::INVALID_CAP_ID;
        dev.state = DeviceState::STARTING;

        if (dev.exit_notif_cap != oz::INVALID_CAP_ID) {
            std::size_t node_idx = dev.dev_id / 64;
            std::size_t slot_idx = dev.dev_id % 64;
            if (node_idx < MAX_TREE_NODES) {
                oz::sys_notification_bind_parent(dev.exit_notif_cap, g_tree_nodes[node_idx], 1ULL << slot_idx);
            }
        }

        oz::sys_write("[DrivMan] Driver spawned: ");
        oz::sys_write(dev.driver_binary);
        oz::sys_write(" (pid=");
        char pbuf[16];
        to_hex_str(dev.pid, pbuf);
        oz::sys_write(&pbuf[14]);
        oz::sys_write(")\n\r");
        return true;
    } else {
        oz::sys_write("[DrivMan] ERROR: ProcMan failed to spawn driver: ");
        oz::sys_write(dev.driver_binary);
        oz::sys_write("\n\r");
        dev.state = DeviceState::FAILED;
        return false;
    }
}

static void handle_driver_crash(std::uint32_t dev_id) {
    DeviceEntry* dev_ptr = find_device_by_id(dev_id);
    if (!dev_ptr || dev_ptr->state == DeviceState::FREE) return;
    DeviceEntry& dev = *dev_ptr;

    oz::sys_write("[DrivMan] CRASH DETECTED: dev_id=");
    char dbuf[16];
    to_hex_str(dev_id, dbuf);
    oz::sys_write(&dbuf[14]);
    oz::sys_write(" (driver: ");
    oz::sys_write(dev.driver_binary ? dev.driver_binary : "unknown");
    oz::sys_write(")\n\r");

    dev.state = DeviceState::FAULTED;

    // 1. PCI FLR / Bus Master Disable (Hardware DMA halt to protect physical memory)
    if (dev.vendor_id != 0 && dev.vendor_id != 0xFFFF) {
        std::uint16_t cmd = pci_config_read16(dev.bus, dev.dev, dev.func, 0x04);
        pci_config_write16(dev.bus, dev.dev, dev.func, 0x04, cmd & ~0x04);
        oz::sys_write("[DrivMan] PCI Bus Master disabled.\n\r");
    }

    // 2. Revoke MMIO Cap
    if (dev.mmio_caps[0] != 0 && dev.mmio_caps[0] != oz::INVALID_CAP_ID) {
        oz::sys_cap_revoke(dev.mmio_caps[0]);
        oz::sys_write("[DrivMan] Revoked MMIO capabilities.\n\r");
    }

    // 3. ProcMan REAP_DRIVER(pid)
    if (CAP_PROCMAN_SEND != 0 && dev.pid != 0) {
        oz::IPCMessage reap_req{};
        reap_req.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::REAP_DRIVER);
        reap_req.data[1] = dev.pid;
        oz::IPCMessage reap_resp{};
        oz::sys_call(CAP_PROCMAN_SEND, &reap_req, &reap_resp);
        oz::sys_write("[DrivMan] Reaped old process from ProcMan.\n\r");
    }

    // Clean old exit notif cap
    if (dev.exit_notif_cap != oz::INVALID_CAP_ID) {
        oz::sys_notification_unbind_parent(dev.exit_notif_cap);
        oz::sys_cap_delete(dev.exit_notif_cap);
        dev.exit_notif_cap = oz::INVALID_CAP_ID;
    }

    // Clean old irq handler cap
    if (dev.irq_handler_cap != oz::INVALID_CAP_ID) {
        oz::sys_cap_revoke(dev.irq_handler_cap);
        oz::sys_cap_delete(dev.irq_handler_cap);
        dev.irq_handler_cap = oz::INVALID_CAP_ID;
    }

    // 4. Circuit Breaker / Crash limit
    std::uint64_t now_tsc = get_tsc();
    dev.crash_count++;
    dev.last_crash_tsc = now_tsc;
    if (dev.crash_count >= 3) {
        dev.state = DeviceState::QUARANTINED;
        oz::sys_write("[DrivMan] Circuit Breaker: Driver crashed >= 3 times, QUARANTINED!\n\r");
        return;
    }

    // 5. Generate reconnect token
    g_token_seed = g_token_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    dev.reconnect_token = g_token_seed ? g_token_seed : 1;

    // 6. Notify VFS of suspension if VFS channel is available
    if (CAP_VFS_SEND != 0) {
        oz::IPCMessage susp_msg{};
        susp_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsCmd::NOTIFY_DEVICE_SUSPENDED);
        susp_msg.data[1] = dev_id;
        susp_msg.data[2] = 1; // CRASH_RECOVERY
        susp_msg.data[3] = dev.reconnect_token;
        oz::IPCMessage susp_resp{};
        oz::sys_call(CAP_VFS_SEND, &susp_msg, &susp_resp);
    }

    // 7. Restart driver
    oz::sys_write("[DrivMan] Self-Healing: Restarting driver...\n\r");
    spawn_driver(dev);
}

static void process_ipc_message(oz::IPCMessage& req, oz::IPCMessage& resp, const volatile oz::BootInfo* boot_info) {
    auto cmd = static_cast<oz::ipc::DrivCmd>(req.data[0]);
    resp = oz::IPCMessage{};

    oz::sys_write("[DrivMan] Received IPC command: ");
    char num[16];
    to_hex_str(static_cast<std::uint64_t>(cmd), num);
    oz::sys_write(&num[14]);
    oz::sys_write("\n\r");

    if (cmd == oz::ipc::DrivCmd::INIT_DEVICE_SESSION) {
        // Transmitter dev_id is authenticated by IPC badge
        std::uint32_t dev_id = static_cast<std::uint32_t>(req.badge);
        DeviceEntry* dev = find_device_by_id(dev_id);
        if (dev) {
            resp.data[0] = 0; // Success
            resp.data[1] = (dev->num_bars & 0xFF) |
                           ((dev->irq_type & 0xFF) << 8) |
                           ((static_cast<std::uint64_t>(dev->instance_id) & 0xFFFF) << 16);
            resp.data[2] = dev->vendor_id | (static_cast<std::uint64_t>(dev->device_id) << 16);
            resp.data[3] = dev->reconnect_token;
            oz::sys_write("[DrivMan] Initialized device session for dev_id=");
            char num[16];
            to_hex_str(dev_id, num);
            oz::sys_write(&num[14]);
            oz::sys_write("\n\r");
        } else {
            resp.data[0] = 1; // Device not found
        }
    } else if (cmd == oz::ipc::DrivCmd::GET_DEVICE_RESOURCE) {
        std::uint32_t dev_id = static_cast<std::uint32_t>(req.badge);
        DeviceEntry* dev = find_device_by_id(dev_id);
        std::uint64_t res_type = req.data[1]; // 1=MMIO, 2=IRQ, 3=FB
        std::uint64_t res_idx = req.data[2];
        oz::sys_write("[DrivMan] GET_DEVICE_RESOURCE dev_id=");
        char num[16];
        to_hex_str(dev_id, num);
        oz::sys_write(&num[14]);
        oz::sys_write(" type=");
        to_hex_str(res_type, num);
        oz::sys_write(&num[14]);
        oz::sys_write("\n\r");

        if (dev) {
            if (res_type == 1) { // MMIO_BAR
                if (res_idx < 6 && dev->mmio_caps[res_idx] != 0) {
                    if (oz::sys_cap_copy(TEMP_XFER_SLOT, dev->mmio_caps[res_idx], static_cast<std::uint32_t>(oz::CapRights::ALL)) == 0) {
                        resp.data[0] = 0;
                        resp.data[1] = dev->mmio_phys[res_idx];
                        resp.data[2] = dev->mmio_size[res_idx];
                        resp.has_cap = true;
                        resp.transferred_cap = TEMP_XFER_SLOT;
                    } else {
                        resp.data[0] = 2; // Cap copy failed
                    }
                } else {
                    resp.data[0] = 1; // Invalid BAR index
                }
            } else if (res_type == 3) { // FRAMEBUFFER
                if (oz::sys_cap_copy(TEMP_XFER_SLOT, oz::CAP_FRAMEBUFFER, static_cast<std::uint32_t>(oz::CapRights::ALL)) == 0) {
                    resp.data[0] = 0;
                    resp.data[1] = boot_info ? boot_info->fb_phys_addr : 0;
                    resp.data[2] = boot_info ? boot_info->fb_size : 0;
                    resp.data[3] = boot_info ? ((static_cast<std::uint64_t>(boot_info->fb_width) << 32) | boot_info->fb_height) : 0;
                    resp.has_cap = true;
                    resp.transferred_cap = TEMP_XFER_SLOT;
                } else {
                    resp.data[0] = 1;
                }
            } else if (res_type == 2) { // IRQ_HANDLER
                if (req.has_cap && req.transferred_cap != oz::INVALID_CAP_ID) {
                    if (dev->irq_handler_cap != oz::INVALID_CAP_ID) {
                        oz::sys_cap_revoke(dev->irq_handler_cap);
                        oz::sys_cap_delete(dev->irq_handler_cap);
                        dev->irq_handler_cap = oz::INVALID_CAP_ID;
                    }
                    std::uint32_t irq_handler = oz::sys_irq_handler_create(oz::CAP_UNTYPED, 0x40, req.transferred_cap);
                    oz::sys_cap_delete(req.transferred_cap);
                    if (irq_handler != oz::INVALID_CAP_ID) {
                        dev->irq_handler_cap = oz::INVALID_CAP_ID;
                        oz::sys_cap_delete(TEMP_XFER_SLOT);
                        if (oz::sys_cap_move(TEMP_XFER_SLOT, irq_handler) == 0) {
                            resp.data[0] = 0;
                            resp.has_cap = true;
                            resp.transferred_cap = TEMP_XFER_SLOT;
                        } else {
                            oz::sys_cap_delete(irq_handler);
                            resp.data[0] = 2;
                        }
                    } else {
                        resp.data[0] = 2;
                    }
                } else {
                    resp.data[0] = 1;
                }
            } else {
                resp.data[0] = 0xFE;
            }
        } else {
            resp.data[0] = 1;
        }
    } else if (cmd == oz::ipc::DrivCmd::NOTIFY_DEVICE_ONLINE) {
        std::uint32_t dev_id = static_cast<std::uint32_t>(req.badge);
        DeviceEntry* dev = find_device_by_id(dev_id);
        if (dev) {
            dev->state = DeviceState::ONLINE;
            dev->reconnect_token = 0;
            resp.data[0] = 0;
            oz::sys_write("[DrivMan] Device ONLINE: dev_id=");
            char num[16];
            to_hex_str(dev_id, num);
            oz::sys_write(&num[14]);
            oz::sys_write("\n\r");
        } else {
            resp.data[0] = 1;
        }
    } else if (cmd == oz::ipc::DrivCmd::GET_DEVICE_CAP) {
        // Phase 1 legacy compatibility
        char dev_name[25] = {};
        const char* src = reinterpret_cast<const char*>(&req.data[1]);
        for (int i = 0; i < 24 && src[i] != '\0'; ++i) {
            dev_name[i] = src[i];
        }

        if (match_name(dev_name, "ahci")) {
            if (oz::sys_cap_copy(TEMP_XFER_SLOT, oz::CAP_AHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL)) == 0) {
                resp.data[0] = 0;
                resp.data[1] = boot_info ? boot_info->ahci_abar_phys : 0;
                resp.has_cap = true;
                resp.transferred_cap = TEMP_XFER_SLOT;
                oz::sys_write("[DrivMan] Brokered AHCI MMIO capability to driver.\n\r");
            } else {
                resp.data[0] = 1;
            }
        } else if (match_name(dev_name, "xhci")) {
            if (oz::sys_cap_copy(TEMP_XFER_SLOT, oz::CAP_XHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL)) == 0) {
                resp.data[0] = 0;
                resp.data[1] = boot_info ? boot_info->xhci_bar_phys : 0;
                resp.has_cap = true;
                resp.transferred_cap = TEMP_XFER_SLOT;
                oz::sys_write("[DrivMan] Brokered xHCI MMIO capability to driver.\n\r");
            } else {
                resp.data[0] = 1;
            }
        } else if (match_name(dev_name, "xhci_irq")) {
            if (req.has_cap && req.transferred_cap != oz::INVALID_CAP_ID) {
                DeviceEntry* xhci_dev = nullptr;
                for (std::size_t i = 0; i < g_num_devices; ++i) {
                    if (g_devices[i].class_code == 0x0C && g_devices[i].subclass == 0x03 && g_devices[i].prog_if == 0x30) {
                        xhci_dev = &g_devices[i];
                        break;
                    }
                }
                if (xhci_dev && xhci_dev->irq_handler_cap != oz::INVALID_CAP_ID) {
                    oz::sys_cap_revoke(xhci_dev->irq_handler_cap);
                    oz::sys_cap_delete(xhci_dev->irq_handler_cap);
                    xhci_dev->irq_handler_cap = oz::INVALID_CAP_ID;
                }
                std::uint32_t irq_handler = oz::sys_irq_handler_create(oz::CAP_UNTYPED, 0x40, req.transferred_cap);
                oz::sys_cap_delete(req.transferred_cap);
                if (irq_handler != oz::INVALID_CAP_ID) {
                    if (xhci_dev) xhci_dev->irq_handler_cap = oz::INVALID_CAP_ID;
                    oz::sys_cap_delete(TEMP_XFER_SLOT);
                    if (oz::sys_cap_move(TEMP_XFER_SLOT, irq_handler) == 0) {
                        resp.data[0] = 0;
                        resp.has_cap = true;
                        resp.transferred_cap = TEMP_XFER_SLOT;
                    } else {
                        oz::sys_cap_delete(irq_handler);
                        resp.data[0] = 2;
                    }
                    oz::sys_write("[DrivMan] Brokered xHCI IRQ handler capability to driver.\n\r");
                } else {
                    resp.data[0] = 2;
                }
            } else {
                resp.data[0] = 1;
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
            } else {
                resp.data[0] = 1;
            }
        } else {
            resp.data[0] = 0xFE;
        }
    } else if (cmd == oz::ipc::DrivCmd::GET_BOOT_INFO) {
        oz::sys_write("[DrivMan] Brokered BootInfo capability to driver.\n\r");
        if (oz::sys_cap_copy(TEMP_XFER_SLOT, oz::CAP_BOOTINFO, static_cast<std::uint32_t>(oz::CapRights::ALL)) == 0) {
            resp.data[0] = 0;
            resp.has_cap = true;
            resp.transferred_cap = TEMP_XFER_SLOT;
        } else {
            resp.data[0] = 1;
        }
    } else {
        oz::sys_write("[DrivMan] ERROR: Unknown IPC command received.\n\r");
        resp.data[0] = 0xFF;
    }
}

static void drivman_ipc_worker() {
    oz::ReplyToken tok = oz::INVALID_REPLY_TOKEN;
    oz::IPCMessage req{};
    oz::IPCMessage resp{};

    while (true) {
        tok = oz::sys_reply_recv(tok, CAP_DRIVMAN_RECV, (tok != oz::INVALID_REPLY_TOKEN) ? &resp : nullptr, &req);
        if (tok == oz::INVALID_REPLY_TOKEN) {
            continue;
        }

        resp = oz::IPCMessage{};
        process_ipc_message(req, resp, g_boot_info);
    }
}

extern "C" void _start() {
    oz::sys_write("[DrivMan] Device Manager server started (Supervisor Model)!\n\r");

    // Map BootInfo Frame (slot 243) if present
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_BOOTINFO, BOOTINFO_VADDR, 0x00, 4096) == 0) {
        g_boot_info = reinterpret_cast<const volatile oz::BootInfo*>(BOOTINFO_VADDR);
    }

    // Initialize Radix-64 Notification Tree
    g_root_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    for (std::size_t n = 0; n < MAX_TREE_NODES; ++n) {
        g_tree_nodes[n] = oz::sys_notification_create(oz::CAP_UNTYPED);
        oz::sys_notification_bind_parent(g_tree_nodes[n], g_root_notif, 1ULL << n);
    }

    // Spawn passive IPC worker thread for CAP_DRIVMAN_RECV (Driver IPC requests)
    std::uint32_t worker_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t worker_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 2);
    static constexpr std::uint64_t WORKER_STACK_TOP = 0x700010000000ULL;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, worker_stack, WORKER_STACK_TOP - 8192, 0x01, 8192);
    oz::sys_tcb_configure(worker_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(drivman_ipc_worker), WORKER_STACK_TOP - 8);
    oz::sys_tcb_resume(worker_tcb);

    // Initial hand-off to let worker reach sys_reply_recv(token=INVALID_REPLY_TOKEN) and unbind
    std::uint32_t worker_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    oz::sys_tcb_bind_baton(worker_tcb, worker_baton);
    oz::sys_baton_inject(worker_baton, 10000);
    oz::SchedResult w_res = oz::sys_baton_switch(worker_baton);
    if (w_res.reason == oz::SchedReason::ORPHANED || w_res.reason == oz::SchedReason::BLOCKED) {
        oz::sys_write("[DrivMan] IPC worker initialized and passive.\n\r");
    }
    oz::sys_cap_delete(worker_baton);

    // Scan PCI bus and register devices
    scan_pci_bus(g_boot_info);

    // Spawn detected drivers via ProcMan (Factory)
    for (std::size_t i = 0; i < g_num_devices; ++i) {
        if (g_devices[i].state == DeviceState::DETECTED) {
            spawn_driver(g_devices[i]);
        }
    }

    oz::sys_write("[DrivMan] Supervisor event loop active.\n\r");

    while (true) {
        std::uint64_t badge = 0;
        int wait_res = oz::sys_notification_wait(g_root_notif, &badge);
        if (wait_res != 0 || badge == 0) continue;

        // Radix-64 Driver Exit / Crash notifications
        for (std::size_t n = 0; n < MAX_TREE_NODES; ++n) {
            if (badge & (1ULL << n)) {
                std::uint64_t node_badge = 0;
                if (oz::sys_notification_poll(g_tree_nodes[n], &node_badge) == 0 && node_badge != 0) {
                    while (node_badge != 0) {
                        int slot = __builtin_ctzll(node_badge);
                        node_badge &= ~(1ULL << slot);
                        std::uint32_t crashed_dev = static_cast<std::uint32_t>(n * 64 + slot);
                        handle_driver_crash(crashed_dev);
                    }
                }
            }
        }
    }
}
