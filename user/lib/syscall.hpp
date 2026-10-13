#pragma once
#include <cstdint>
#include <cstddef>

namespace oz {

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
bool sys_channel_create(std::uint32_t* out_cap0, std::uint32_t* out_cap1);

// Capability-based memory management
int sys_vspace_map(std::uint32_t vspace_cap, std::uint32_t frame_cap,
                   std::uint64_t virt_addr, std::uint64_t flags);
std::uint32_t sys_frame_alloc(std::uint32_t untyped_cap, std::uint64_t num_pages);
int sys_vspace_unmap(std::uint32_t vspace_cap, std::uint64_t virt_addr);
std::uint64_t sys_frame_phys_addr(std::uint32_t frame_cap);

void sys_debug_print(const char* str);

// Capability management
std::uint32_t sys_cap_mint(std::uint32_t src_cap, std::uint32_t new_rights);
int sys_cap_delete(std::uint32_t cap_id);

// Process/thread management
int sys_tcb_resume(std::uint32_t tcb_cap);
int sys_tcb_suspend(std::uint32_t tcb_cap);
std::uint32_t sys_cspace_create();
int sys_cspace_cap_insert(std::uint32_t cspace_cap, std::uint32_t target_slot,
                          std::uint32_t src_cap, std::uint32_t rights);
std::uint32_t sys_vspace_create();

// Hardware resource management
std::uint32_t sys_irq_handler_create(std::uint8_t vector, std::uint32_t notification_ep_cap);
int sys_irq_ack(std::uint32_t irq_handler_cap);
std::uint32_t sys_ioport_create(std::uint16_t base_port, std::uint16_t port_count);
std::uint64_t sys_ioport_read(std::uint32_t ioport_cap, std::uint16_t port, std::uint32_t width);
int sys_ioport_write(std::uint32_t ioport_cap, std::uint16_t port, std::uint64_t value, std::uint32_t width);

}

// Well-known capability slot constants
static constexpr std::uint32_t CAP_SELF_VSPACE = 0;
static constexpr std::uint32_t CAP_UNTYPED = 240;

} // namespace oz
