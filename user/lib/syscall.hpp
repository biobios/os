#pragma once
#include <cstdint>
#include <cstddef>

namespace oz {

enum class CapRights : std::uint32_t {
    NONE      = 0,
    READ      = 1 << 0,
    WRITE     = 1 << 1,
    EXEC      = 1 << 2,
    GRANT     = 1 << 3,
    DUPLICATE = 1 << 4,
    ONCE      = 1 << 5,
    ALL       = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4)
};

inline constexpr CapRights operator|(CapRights a, CapRights b) {
    return static_cast<CapRights>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline constexpr CapRights operator&(CapRights a, CapRights b) {
    return static_cast<CapRights>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

inline constexpr CapRights operator~(CapRights a) {
    return static_cast<CapRights>(~static_cast<std::uint32_t>(a));
}

enum class ObjectType : std::uint32_t {
    Unknown = 0,
    ChannelEndpoint = 1,
    Frame = 2,
    UntypedMemory = 3,
    VSpace = 4,
    TCB = 5,
    CSpaceCap = 6,
    IRQHandler = 7,
    IOPort = 8,
    PageTable = 9,
    Notification = 10,
};

extern "C" {

struct IPCMessage {
    std::uint64_t data[4];
    std::uint32_t transferred_cap;
    bool has_cap{false};
};

void sys_exit(int status);

std::size_t sys_write(const char* str);

bool sys_channel_send(std::uint32_t cap_id, const IPCMessage* msg);
bool sys_channel_recv(std::uint32_t cap_id, IPCMessage* msg);
bool sys_channel_create(std::uint32_t untyped_cap, std::uint32_t* out_cap0, std::uint32_t* out_cap1);
bool sys_channel_create_ext(std::uint32_t untyped_cap, std::uint32_t rights0, std::uint32_t rights1, std::uint32_t* out_cap0, std::uint32_t* out_cap1);
bool sys_call(std::uint32_t cap_id, const IPCMessage* send_msg, IPCMessage* recv_msg);
bool sys_reply_recv(std::uint32_t cap_id, const IPCMessage* reply_msg, IPCMessage* recv_msg);

// Capability-based memory management
int sys_vspace_map(std::uint32_t vspace_cap, std::uint32_t frame_cap,
                   std::uint64_t virt_addr, std::uint64_t flags);
int sys_vspace_map_pagetable(std::uint32_t vspace_cap, std::uint32_t pagetable_cap,
                             std::uint64_t virt_addr, std::uint32_t level);
int sys_vspace_ensure_pagetables_range(std::uint32_t vspace_cap, std::uint32_t untyped_cap,
                                       std::uint64_t start_vaddr, std::uint64_t size);
int sys_vspace_map_with_pagetables(std::uint32_t vspace_cap, std::uint32_t untyped_cap,
                                   std::uint32_t frame_cap, std::uint64_t virt_addr,
                                   std::uint64_t flags, std::uint64_t size);
std::uint32_t sys_frame_alloc(std::uint32_t untyped_cap, std::uint64_t num_pages);
int sys_vspace_unmap(std::uint32_t vspace_cap, std::uint64_t virt_addr);
std::uint64_t sys_frame_phys_addr(std::uint32_t frame_cap);
std::uint64_t sys_frame_size(std::uint32_t frame_cap);

std::uint32_t sys_untyped_retype(std::uint32_t untyped_cap, std::uint32_t target_type, std::uint64_t arg);
int sys_untyped_revoke(std::uint32_t untyped_cap);

void sys_debug_print(const char* str);

// Capability management
std::uint32_t sys_cap_mint(std::uint32_t src_cap, std::uint32_t new_rights);
int sys_cap_delete(std::uint32_t cap_id);
int sys_cap_move(std::uint32_t dest_slot, std::uint32_t src_slot);
int sys_cspace_cap_move(std::uint32_t target_cspace_cap, std::uint32_t target_slot, std::uint32_t src_slot);
int sys_cap_copy(std::uint32_t dest_slot, std::uint32_t src_slot, std::uint32_t rights);

// Process/thread management
int sys_tcb_configure(std::uint32_t tcb_cap, std::uint32_t cspace_cap,
                      std::uint32_t vspace_cap, std::uint64_t rip,
                      std::uint64_t user_rsp);
int sys_tcb_resume(std::uint32_t tcb_cap);
int sys_tcb_suspend(std::uint32_t tcb_cap);
int sys_tcb_set_fault_endpoint(std::uint32_t tcb_cap, std::uint32_t ep_cap);
std::uint32_t sys_cspace_create(std::uint32_t untyped_cap);
int sys_cspace_cap_insert(std::uint32_t cspace_cap, std::uint32_t target_slot,
                          std::uint32_t src_cap, std::uint32_t rights);
std::uint32_t sys_vspace_create(std::uint32_t untyped_cap);

// Hardware resource management
std::uint32_t sys_irq_handler_create(std::uint32_t untyped_cap, std::uint8_t vector, std::uint32_t notification_ep_cap);
int sys_irq_ack(std::uint32_t irq_handler_cap);
std::uint32_t sys_ioport_create(std::uint32_t parent_ioport_cap, std::uint32_t untyped_cap, std::uint16_t base_port, std::uint32_t port_count);
std::uint64_t sys_ioport_read(std::uint32_t ioport_cap, std::uint16_t port, std::uint32_t width);
int sys_ioport_write(std::uint32_t ioport_cap, std::uint16_t port, std::uint64_t value, std::uint32_t width);

// Notification management
std::uint32_t sys_notification_create(std::uint32_t untyped_cap);
int sys_notification_signal(std::uint32_t notif_cap, std::uint64_t badge);
int sys_notification_wait(std::uint32_t notif_cap, std::uint64_t* out_badge);
int sys_channel_bind_notification(std::uint32_t ep_cap, std::uint32_t notif_cap, std::uint64_t badge);
int sys_notification_poll(std::uint32_t notif_cap, std::uint64_t* out_badge);

}

// Well-known capability slot constants
static constexpr std::uint32_t CAP_SELF_VSPACE = 0;
static constexpr std::uint32_t CAP_UNTYPED = 240;
static constexpr std::uint32_t CAP_AHCI_MMIO = 241;
static constexpr std::uint32_t CAP_XHCI_MMIO = 242;
static constexpr std::uint32_t CAP_BOOTINFO = 243;
static constexpr std::uint32_t CAP_INITRD = 244;
static constexpr std::uint32_t CAP_SELF_CSPACE = 245;
static constexpr std::uint32_t CAP_FRAMEBUFFER = 246;
static constexpr std::uint32_t CAP_ROOT_IOPORT = 247;
static constexpr std::uint32_t INVALID_CAP_ID = 0xFFFFFFFF;

// ============================================================================
// C++ RAII helpers for Linear & Once Capabilities
// ============================================================================

template <typename Tag = void>
class LinearCap {
private:
    std::uint32_t slot_{INVALID_CAP_ID};

public:
    LinearCap() = default;
    explicit LinearCap(std::uint32_t slot) : slot_(slot) {}

    ~LinearCap() {
        reset();
    }

    LinearCap(const LinearCap&) = delete;
    LinearCap& operator=(const LinearCap&) = delete;

    LinearCap(LinearCap&& other) noexcept : slot_(other.slot_) {
        other.slot_ = INVALID_CAP_ID;
    }

    LinearCap& operator=(LinearCap&& other) noexcept {
        if (this != &other) {
            reset();
            slot_ = other.slot_;
            other.slot_ = INVALID_CAP_ID;
        }
        return *this;
    }

    bool isValid() const { return slot_ != INVALID_CAP_ID; }
    std::uint32_t get() const { return slot_; }

    std::uint32_t release() {
        std::uint32_t s = slot_;
        slot_ = INVALID_CAP_ID;
        return s;
    }

    void reset(std::uint32_t new_slot = INVALID_CAP_ID) {
        if (slot_ != INVALID_CAP_ID) {
            sys_cap_delete(slot_);
        }
        slot_ = new_slot;
    }

    // Move capability to another slot
    bool moveTo(std::uint32_t dest_slot) {
        if (!isValid()) return false;
        if (sys_cap_move(dest_slot, slot_) == 0) {
            slot_ = dest_slot;
            return true;
        }
        return false;
    }
};

// One-time sender wrapper (automatically invalidates after sending)
class OnceSender {
private:
    std::uint32_t slot_{INVALID_CAP_ID};

public:
    OnceSender() = default;
    explicit OnceSender(std::uint32_t slot) : slot_(slot) {}

    ~OnceSender() {
        if (slot_ != INVALID_CAP_ID) {
            sys_cap_delete(slot_);
        }
    }

    OnceSender(const OnceSender&) = delete;
    OnceSender& operator=(const OnceSender&) = delete;

    OnceSender(OnceSender&& other) noexcept : slot_(other.slot_) {
        other.slot_ = INVALID_CAP_ID;
    }

    OnceSender& operator=(OnceSender&& other) noexcept {
        if (this != &other) {
            if (slot_ != INVALID_CAP_ID) sys_cap_delete(slot_);
            slot_ = other.slot_;
            other.slot_ = INVALID_CAP_ID;
        }
        return *this;
    }

    bool isValid() const { return slot_ != INVALID_CAP_ID; }
    std::uint32_t get() const { return slot_; }

    bool send(const IPCMessage& msg) {
        if (slot_ == INVALID_CAP_ID) return false;
        bool res = sys_channel_send(slot_, &msg);
        // Consumed by kernel on send; invalidate user-space handle as well
        slot_ = INVALID_CAP_ID;
        return res;
    }
};

} // namespace oz
