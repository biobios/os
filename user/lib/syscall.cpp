#include "syscall.hpp"

namespace oz {

extern "C" {

std::uint64_t syscall(std::uint64_t number, std::uint64_t arg1, std::uint64_t arg2, std::uint64_t arg3, std::uint64_t arg4, std::uint64_t arg5, std::uint64_t arg6) {
    std::uint64_t ret;
    register std::uint64_t r10 __asm__("r10") = arg4;
    register std::uint64_t r8  __asm__("r8")  = arg5;
    register std::uint64_t r9  __asm__("r9")  = arg6;
    
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void sys_exit(int status) {
    syscall(60, static_cast<std::uint64_t>(status), 0, 0, 0, 0, 0);
    while (1) {
        __asm__ volatile("pause");
    }
}

bool sys_channel_send(std::uint32_t cap_id, const IPCMessage* msg) {
    return syscall(1, cap_id, reinterpret_cast<std::uint64_t>(msg), 0, 0, 0, 0) == 0;
}

bool sys_channel_recv(std::uint32_t cap_id, IPCMessage* msg) {
    return syscall(2, cap_id, reinterpret_cast<std::uint64_t>(msg), 0, 0, 0, 0) == 0;
}

bool sys_channel_create(std::uint32_t* out_cap0, std::uint32_t* out_cap1) {
    return syscall(3, reinterpret_cast<std::uint64_t>(out_cap0), reinterpret_cast<std::uint64_t>(out_cap1), 0, 0, 0, 0) == 0;
}

std::size_t sys_write(const char* str) {
    IPCMessage msg;
    msg.has_cap = false;
    
    std::size_t i = 0;
    while (str[i] != '\0') {
        alignas(8) char buf[32];
        int j = 0;
        for (; j < 31 && str[i] != '\0'; ++j, ++i) {
            buf[j] = str[i];
        }
        buf[j] = '\0';
        
        for (int k = 0; k < 4; ++k) {
            msg.data[k] = *reinterpret_cast<std::uint64_t*>(&buf[k * 8]);
        }
        
        sys_channel_send(1, &msg);
    }
    return i;
}


int sys_vspace_map(std::uint32_t vspace_cap, std::uint32_t frame_cap,
                   std::uint64_t virt_addr, std::uint64_t flags) {
    return static_cast<int>(syscall(4, vspace_cap, frame_cap, virt_addr, flags, 0, 0));
}

std::uint32_t sys_frame_alloc(std::uint32_t untyped_cap, std::uint64_t num_pages) {
    return static_cast<std::uint32_t>(syscall(5, untyped_cap, num_pages, 0, 0, 0, 0));
}

int sys_vspace_unmap(std::uint32_t vspace_cap, std::uint64_t virt_addr) {
    return static_cast<int>(syscall(7, vspace_cap, virt_addr, 0, 0, 0, 0));
}

std::uint64_t sys_frame_phys_addr(std::uint32_t frame_cap) {
    return syscall(8, frame_cap, 0, 0, 0, 0, 0);
}

void sys_debug_print(const char* str) {
    syscall(6, reinterpret_cast<std::uint64_t>(str), 0, 0, 0, 0, 0);
}

std::uint32_t sys_cap_mint(std::uint32_t src_cap, std::uint32_t new_rights) {
    return static_cast<std::uint32_t>(syscall(9, src_cap, new_rights, 0, 0, 0, 0));
}

int sys_cap_delete(std::uint32_t cap_id) {
    return static_cast<int>(syscall(10, cap_id, 0, 0, 0, 0, 0));
}

int sys_tcb_resume(std::uint32_t tcb_cap) {
    return static_cast<int>(syscall(13, tcb_cap, 0, 0, 0, 0, 0));
}

int sys_tcb_suspend(std::uint32_t tcb_cap) {
    return static_cast<int>(syscall(14, tcb_cap, 0, 0, 0, 0, 0));
}

std::uint32_t sys_cspace_create() {
    return static_cast<std::uint32_t>(syscall(15, 0, 0, 0, 0, 0, 0));
}

int sys_cspace_cap_insert(std::uint32_t cspace_cap, std::uint32_t target_slot,
                          std::uint32_t src_cap, std::uint32_t rights) {
    return static_cast<int>(syscall(16, cspace_cap, target_slot, src_cap, rights, 0, 0));
}

std::uint32_t sys_vspace_create() {
    return static_cast<std::uint32_t>(syscall(17, 0, 0, 0, 0, 0, 0));
}

std::uint32_t sys_irq_handler_create(std::uint8_t vector, std::uint32_t notification_ep_cap) {
    return static_cast<std::uint32_t>(syscall(18, vector, notification_ep_cap, 0, 0, 0, 0));
}

int sys_irq_ack(std::uint32_t irq_handler_cap) {
    return static_cast<int>(syscall(19, irq_handler_cap, 0, 0, 0, 0, 0));
}

std::uint32_t sys_ioport_create(std::uint16_t base_port, std::uint16_t port_count) {
    return static_cast<std::uint32_t>(syscall(20, base_port, port_count, 0, 0, 0, 0));
}

std::uint64_t sys_ioport_read(std::uint32_t ioport_cap, std::uint16_t port, std::uint32_t width) {
    return syscall(21, ioport_cap, port, width, 0, 0, 0);
}

int sys_ioport_write(std::uint32_t ioport_cap, std::uint16_t port, std::uint64_t value, std::uint32_t width) {
    return static_cast<int>(syscall(22, ioport_cap, port, value, width, 0, 0));
}

} // extern "C"

} // namespace oz
