#include "core/Syscall.hpp"
#include "hardware/x86_64.hpp"
#include "core/CapOperations.hpp"
#include "core/Thread.hpp"
#include "utils/utils.hpp"
#include "core/Frame.hpp"
#include "core/VSpace.hpp"
#include "core/UntypedMemory.hpp"
#include "core/TCB.hpp"
#include "core/CSpaceCap.hpp"
#include "core/PageTableObject.hpp"
#include "core/IRQHandler.hpp"
#include "core/IOPort.hpp"

#include "core/Kernel.hpp"

namespace oz {
void (*g_exit_thread_func)() = nullptr;
CSpace* (*g_get_current_cspace_func)() = nullptr;
Thread* (*g_get_current_thread_func)() = nullptr;
void (*g_block_thread_func)() = nullptr;
void (*g_unblock_thread_func)(Thread*) = nullptr;
void (*g_queue_thread_func)(Thread* t) = nullptr;
void (*g_register_irq_handler_fn)(std::uint8_t, IRQHandler*) = nullptr;
void (*g_eoi_fn)() = nullptr;
std::uint8_t (*g_read_io8_fn)(std::uint16_t) = nullptr;
std::uint16_t (*g_read_io16_fn)(std::uint16_t) = nullptr;
std::uint32_t (*g_read_io32_fn)(std::uint16_t) = nullptr;
void (*g_write_io8_fn)(std::uint16_t, std::uint8_t) = nullptr;
void (*g_write_io16_fn)(std::uint16_t, std::uint16_t) = nullptr;
void (*g_write_io32_fn)(std::uint16_t, std::uint32_t) = nullptr;


SyscallContext g_syscall_context;

extern "C" void syscall_entry();

// ===========================================================================
// Phase 5: Security helpers
// ===========================================================================

// User-space pointer validation
// x86-64 canonical lower-half: addresses < 0x0000800000000000 are user-space
static constexpr std::uintptr_t USER_SPACE_LIMIT = 0x0000800000000000ULL;

static bool is_user_address(std::uintptr_t addr) {
    return addr != 0 && addr < USER_SPACE_LIMIT;
}

static bool is_user_buffer(std::uintptr_t addr, std::size_t size) {
    if (addr == 0 || size == 0) return false;
    if (addr >= USER_SPACE_LIMIT) return false;
    if (addr + size < addr) return false;  // overflow check
    if (addr + size > USER_SPACE_LIMIT) return false;
    return true;
}

// Type-safe capability extraction: returns typed pointer or nullptr
template <typename T>
static T* get_cap_typed(CSpace* cspace, CapID id, ObjectType expected, CapRights required) {
    Capability cap = cspace->get(id);
    if (!cap.is_valid()) return nullptr;
    if ((cap.rights & required) != required) return nullptr;
    if (cap.object->objectType() != expected) return nullptr;
    return static_cast<T*>(cap.object);
}

// Get current CSpace (common pattern)
static CSpace* get_cspace() {
    if (!g_get_current_cspace_func) return nullptr;
    return g_get_current_cspace_func();
}

// ===========================================================================
// Syscall handlers
// ===========================================================================

extern "C" std::uint64_t handleSyscall(std::uint64_t rdi, std::uint64_t rsi, std::uint64_t rdx,
                                       std::uint64_t r10, std::uint64_t r8, std::uint64_t r9,
                                       std::uint64_t syscall_number) {
    if (syscall_number == 1) {
        // channel_send
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(rsi, sizeof(IPCMessage))) return -1;
        const IPCMessage* msg = reinterpret_cast<const IPCMessage*>(rsi);
        
        ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
            cspace, static_cast<CapID>(rdi), ObjectType::ChannelEndpoint, CapRights::WRITE);
        if (!ep || ep->is_peer_closed()) return -1;

        // Build internal message with optional cap attachment
        InternalIPCMessage internal;
        internal.user_msg = *msg;
        
        if (msg->has_cap) {
            // Transfer requires GRANT right on the endpoint
            Capability ep_cap_check = cspace->get(static_cast<CapID>(rdi));
            if ((ep_cap_check.rights & CapRights::GRANT) == CapRights::NONE) return -1;
            
            // Extract the capability from sender's CSpace
            Capability src_cap = cspace->get(msg->transferred_cap);
            if (!src_cap.is_valid()) return -1;
            
            // Attach cap to internal message (copy semantics for refcount)
            internal.attached_cap = src_cap;
            
            // Remove from sender's CSpace (move semantics)
            cspace->remove(msg->transferred_cap);
        }

        if (!ep->send(internal)) return -1;

        // Wake up a thread blocked on recv on the peer endpoint
        Thread* waiter = ep->notify_peer_waiters();
        if (waiter && g_unblock_thread_func) {
            g_unblock_thread_func(waiter);
        }
        
        return 0;
    } else if (syscall_number == 2) {
        // channel_recv
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(rsi, sizeof(IPCMessage))) return -1;
        IPCMessage* msg = reinterpret_cast<IPCMessage*>(rsi);
        
        ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
            cspace, static_cast<CapID>(rdi), ObjectType::ChannelEndpoint, CapRights::READ);
        if (!ep) return -1;
        
        // Helper: process a received internal message (extract cap if present)
        auto deliver_message = [&](InternalIPCMessage& internal) -> bool {
            *msg = internal.user_msg;
            if (internal.attached_cap.is_valid()) {
                // Insert the transferred cap into receiver's CSpace
                CapID new_id = cspace->add(internal.attached_cap);
                if (new_id == INVALID_CAP_ID) {
                    // CSpace is full - cap is lost
                    msg->has_cap = false;
                } else {
                    msg->transferred_cap = new_id;
                    msg->has_cap = true;
                }
            }
            return true;
        };
        
        // Fast path: message already available
        InternalIPCMessage internal;
        if (ep->receive(internal)) {
            deliver_message(internal);
            return 0;
        }
        
        // No message - block the current thread
        if (!g_get_current_thread_func || !g_block_thread_func) return -1;
        
        Thread* current = g_get_current_thread_func();
        if (!current) return -1;
        
        current->state = ThreadState::BlockedOnRecv;
        ep->add_waiter(current);
        
        // This switches to another thread; returns when we're woken up
        g_block_thread_func();
        
        // Woken up by a sender - try to receive the message
        if (ep->receive(internal)) {
            deliver_message(internal);
            return 0;
        }
        
        // Peer may have closed
        return -1;
    } else if (syscall_number == 3) {
        // channel_create
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        if (!is_user_buffer(rdi, sizeof(CapID))) return -1;
        if (!is_user_buffer(rsi, sizeof(CapID))) return -1;
        CapID* out0 = reinterpret_cast<CapID*>(rdi);
        CapID* out1 = reinterpret_cast<CapID*>(rsi);
        if (sys_channel_create(cspace, out0, out1)) return 0;
        return -1;
    } else if (syscall_number == 4) {
        // vspace_map
        CSpace* cspace = get_cspace();
        if (!cspace) { dprint("[vspace_map] no cspace\n\r"); return -1; }
        
        VSpace* vspace = get_cap_typed<VSpace>(
            cspace, static_cast<CapID>(rdi), ObjectType::VSpace, CapRights::WRITE);
        if (!vspace) { dprint("[vspace_map] bad vspace cap\n\r"); return -1; }
        
        Frame* frame = get_cap_typed<Frame>(
            cspace, static_cast<CapID>(rsi), ObjectType::Frame, CapRights::READ);
        if (!frame) { dprint("[vspace_map] bad frame cap\n\r"); return -1; }
        
        std::uintptr_t virt = rdx;
        std::uintptr_t phys = frame->physicalAddress();
        std::size_t frame_size = frame->size();
        for (std::size_t offset = 0; offset < frame_size; offset += 4096) {
            if (!vspace->mapUser(virt + offset, phys + offset, r10)) {
                return -1;
            }
        }
        return 0;
    } else if (syscall_number == 5) {
        // frame_alloc
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rdi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;
        
        Frame* frame = untyped->retypeFrame(rsi);
        if (!frame) return -1;
        
        Capability frame_cap(frame, CapRights::ALL);
        CapID new_id = cspace->add(frame_cap);
        frame->release();
        
        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 60) {
        // exit
        dprint("User process exited.\n\r");
        if (g_exit_thread_func) g_exit_thread_func();
        return 0;
    } else if (syscall_number == 6) {
        // debug_print: write string to serial (secured with pointer validation)
        if (!is_user_address(rdi)) return -1;
        const char* str = reinterpret_cast<const char*>(rdi);
        // Print with length limit to prevent runaway reads
        constexpr std::size_t MAX_DEBUG_LEN = 4096;
        for (std::size_t i = 0; i < MAX_DEBUG_LEN; ++i) {
            if (!is_user_address(rdi + i)) break;
            char c = str[i];
            if (c == '\0') break;
            char buf[2] = {c, '\0'};
            dprint(buf);
        }
        return 0;
    } else if (syscall_number == 7) {
        // vspace_unmap
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        VSpace* vspace = get_cap_typed<VSpace>(
            cspace, static_cast<CapID>(rdi), ObjectType::VSpace, CapRights::WRITE);
        if (!vspace) return -1;
        
        if (!vspace->unmapUser(rsi)) return -1;
        return 0;
    } else if (syscall_number == 8) {
        // frame_phys_addr
        CSpace* cspace = get_cspace();
        if (!cspace) return 0;
        
        Frame* frame = get_cap_typed<Frame>(
            cspace, static_cast<CapID>(rdi), ObjectType::Frame, CapRights::READ);
        if (!frame) return 0;
        
        return static_cast<std::uint64_t>(frame->physicalAddress());
    } else if (syscall_number == 9) {
        // cap_mint
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        Capability src_cap = cspace->get(static_cast<CapID>(rdi));
        if (!src_cap.is_valid()) return -1;
        
        CapRights new_rights = static_cast<CapRights>(rsi);
        if ((src_cap.rights & new_rights) != new_rights) return -1;
        
        Capability minted(src_cap.object, new_rights);
        CapID new_id = cspace->add(minted);
        
        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 10) {
        // cap_delete
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        if (!cspace->remove(static_cast<CapID>(rdi))) return -1;
        return 0;
    } else if (syscall_number == 11) {
        // tcb_configure(tcb_cap, cspace_cap, vspace_cap, rip, user_rsp)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        TCB* tcb = get_cap_typed<TCB>(
            cspace, static_cast<CapID>(rdi), ObjectType::TCB, CapRights::WRITE);
        if (!tcb) return -1;

        CSpaceCap* target_cspace = get_cap_typed<CSpaceCap>(
            cspace, static_cast<CapID>(rsi), ObjectType::CSpaceCap, CapRights::READ);
        if (!target_cspace) return -1;

        VSpace* target_vspace = get_cap_typed<VSpace>(
            cspace, static_cast<CapID>(rdx), ObjectType::VSpace, CapRights::READ);
        if (!target_vspace) return -1;

        if (!is_user_address(r10)) return -1; // rip
        if (!is_user_address(r8)) return -1;  // user_rsp

        if (!tcb->configure(target_cspace->getCSpace(), target_vspace, r10, r8)) {
            return -1;
        }
        return 0;
    } else if (syscall_number == 13) {
        // tcb_resume
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        TCB* tcb = get_cap_typed<TCB>(
            cspace, static_cast<CapID>(rdi), ObjectType::TCB, CapRights::WRITE);
        if (!tcb) return -1;
        
        if (!tcb->resume()) return -1;
        return 0;
    } else if (syscall_number == 14) {
        // tcb_suspend
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        TCB* tcb = get_cap_typed<TCB>(
            cspace, static_cast<CapID>(rdi), ObjectType::TCB, CapRights::WRITE);
        if (!tcb) return -1;
        
        if (!tcb->suspend()) return -1;
        return 0;
    } else if (syscall_number == 15) {
        // cspace_create
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, 240, ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;
        
        CSpaceCap* cs_cap_obj = untyped->retypeCSpace();
        if (!cs_cap_obj) return -1;
        
        Capability cs_cap(cs_cap_obj, CapRights::ALL);
        CapID new_id = cspace->add(cs_cap);
        cs_cap_obj->release();
        
        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 16) {
        // cspace_cap_insert
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        CSpaceCap* target_cs = get_cap_typed<CSpaceCap>(
            cspace, static_cast<CapID>(rdi), ObjectType::CSpaceCap, CapRights::WRITE);
        if (!target_cs) return -1;
        
        Capability src_cap = cspace->get(static_cast<CapID>(rdx));
        if (!src_cap.is_valid()) return -1;
        
        CapRights new_rights = static_cast<CapRights>(r10);
        if ((src_cap.rights & new_rights) != new_rights) return -1;
        
        Capability minted(src_cap.object, new_rights);
        if (!target_cs->insertCap(static_cast<CapID>(rsi), minted)) return -1;
        return 0;
    } else if (syscall_number == 17) {
        // vspace_create
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, 240, ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;
        
        std::uintptr_t pml4_phys = 0;
        void* pml4_mem = untyped->allocateBytes(4096, 4096, pml4_phys);
        if (!pml4_mem) return -1;

        auto user_as = ProcessAddressSpace::createFromPhysical(
            VSpace::createPhysicalAddress<PageTable>(pml4_phys),
            Kernel::instance().getKernelSpace());
        if (!user_as.isValid()) return -1;
        auto* as_obj = untyped->allocateObject<ProcessAddressSpace>(std::move(user_as));
        if (!as_obj) return -1;
        
        auto* vspace = untyped->allocateObject<VSpace>(as_obj, pml4_phys);
        if (!vspace) return -1;
        Capability vs_cap(vspace, CapRights::ALL);
        CapID new_id = cspace->add(vs_cap);
        vspace->release();
        
        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 18) {
        // irq_handler_create
        CSpace* cspace = get_cspace();
        if (!cspace || !g_register_irq_handler_fn || !g_eoi_fn) return -1;
        
        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, 240, ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;
        
        std::uint8_t vector = static_cast<std::uint8_t>(rdi);
        
        ChannelEndpoint* ep = get_cap_typed<ChannelEndpoint>(
            cspace, static_cast<CapID>(rsi), ObjectType::ChannelEndpoint, CapRights::WRITE);
        if (!ep) return -1;
        
        auto* handler = untyped->allocateObject<IRQHandler>(vector, g_register_irq_handler_fn, g_eoi_fn);
        if (!handler) return -1;
        handler->setNotificationEndpoint(ep);
        handler->registerHandler();
        
        Capability irq_cap(handler, CapRights::ALL);
        CapID new_id = cspace->add(irq_cap);
        handler->release();
        
        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 19) {
        // irq_ack
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        IRQHandler* handler = get_cap_typed<IRQHandler>(
            cspace, static_cast<CapID>(rdi), ObjectType::IRQHandler, CapRights::WRITE);
        if (!handler) return -1;
        
        if (!handler->acknowledge()) return -1;
        return 0;
    } else if (syscall_number == 20) {
        // ioport_create
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, 240, ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;
        
        auto* ioport = untyped->allocateObject<IOPort>(
            static_cast<std::uint16_t>(rdi),
            static_cast<std::uint16_t>(rsi),
            g_read_io8_fn, g_read_io16_fn, g_read_io32_fn,
            g_write_io8_fn, g_write_io16_fn, g_write_io32_fn
        );
        if (!ioport) return -1;
        
        Capability io_cap(ioport, CapRights::ALL);
        CapID new_id = cspace->add(io_cap);
        ioport->release();
        
        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 21) {
        // ioport_read
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        IOPort* ioport = get_cap_typed<IOPort>(
            cspace, static_cast<CapID>(rdi), ObjectType::IOPort, CapRights::READ);
        if (!ioport) return -1;
        
        std::uint16_t port = static_cast<std::uint16_t>(rsi);
        if (rdx == 1) {
            std::uint8_t val;
            if (!ioport->read8(port, val)) return -1;
            return static_cast<std::uint64_t>(val);
        } else if (rdx == 2) {
            std::uint16_t val;
            if (!ioport->read16(port, val)) return -1;
            return static_cast<std::uint64_t>(val);
        } else if (rdx == 4) {
            std::uint32_t val;
            if (!ioport->read32(port, val)) return -1;
            return static_cast<std::uint64_t>(val);
        }
        return -1;
    } else if (syscall_number == 22) {
        // ioport_write
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;
        
        IOPort* ioport = get_cap_typed<IOPort>(
            cspace, static_cast<CapID>(rdi), ObjectType::IOPort, CapRights::WRITE);
        if (!ioport) return -1;
        
        std::uint16_t port = static_cast<std::uint16_t>(rsi);
        if (r10 == 1) {
            if (!ioport->write8(port, static_cast<std::uint8_t>(rdx))) return -1;
        } else if (r10 == 2) {
            if (!ioport->write16(port, static_cast<std::uint16_t>(rdx))) return -1;
        } else if (r10 == 4) {
            if (!ioport->write32(port, static_cast<std::uint32_t>(rdx))) return -1;
        } else {
            return -1;
        }
        return 0;
    } else if (syscall_number == 23) {
        // untyped_retype(untyped_cap, target_type, arg)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rdi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;

        ObjectType target_type = static_cast<ObjectType>(rsi);
        KernelObject* obj = nullptr;

        if (target_type == ObjectType::Frame) {
            obj = untyped->retypeFrame(rdx); // rdx = num_pages
        } else if (target_type == ObjectType::PageTable) {
            obj = untyped->retypePageTable();
        } else if (target_type == ObjectType::TCB) {
            obj = untyped->retypeTCB(g_queue_thread_func);
        } else if (target_type == ObjectType::CSpaceCap) {
            obj = untyped->retypeCSpace();
        } else if (target_type == ObjectType::ChannelEndpoint) {
            ChannelEndpoint* end0 = nullptr;
            ChannelEndpoint* end1 = nullptr;
            if (!untyped->retypeChannelPair(&end0, &end1)) return -1;
            Capability cap0(end0, CapRights::ALL);
            Capability cap1(end1, CapRights::ALL);
            CapID id0 = cspace->add(cap0);
            CapID id1 = cspace->add(cap1);
            end0->release();
            end1->release();
            if (id0 == INVALID_CAP_ID || id1 == INVALID_CAP_ID) return -1;
            return (static_cast<std::uint64_t>(id1) << 32) | static_cast<std::uint64_t>(id0);
        }

        if (!obj) return -1;

        Capability cap(obj, CapRights::ALL);
        CapID new_id = cspace->add(cap);
        obj->release();

        if (new_id == INVALID_CAP_ID) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(new_id);
    } else if (syscall_number == 24) {
        // untyped_revoke(untyped_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        UntypedMemory* untyped = get_cap_typed<UntypedMemory>(
            cspace, static_cast<CapID>(rdi), ObjectType::UntypedMemory, CapRights::WRITE);
        if (!untyped) return -1;

        untyped->revoke();
        return 0;
    } else if (syscall_number == 25) {
        // vspace_map_pagetable(vspace_cap, pt_cap, virt_addr, level)
        CSpace* cspace = get_cspace();
        if (!cspace) return -1;

        VSpace* vspace = get_cap_typed<VSpace>(
            cspace, static_cast<CapID>(rdi), ObjectType::VSpace, CapRights::WRITE);
        if (!vspace) return -1;

        PageTableObject* pt_obj = get_cap_typed<PageTableObject>(
            cspace, static_cast<CapID>(rsi), ObjectType::PageTable, CapRights::WRITE);
        if (!pt_obj) return -1;

        if (pt_obj->isMapped()) return -1;

        std::uintptr_t virt = rdx;
        std::uint8_t level = static_cast<std::uint8_t>(r10); // 1: PT, 2: PD, 3: PDPT
        if (level < 1 || level > 3) return -1;

        if (!vspace->installPageTable(virt, pt_obj->physicalAddress(), level)) {
            return -1;
        }

        pt_obj->setMapped(true, level);
        return 0;
    } else if (syscall_number == 26) {
        // frame_size(frame_cap)
        CSpace* cspace = get_cspace();
        if (!cspace) return 0;

        Frame* frame = get_cap_typed<Frame>(
            cspace, static_cast<CapID>(rdi), ObjectType::Frame, CapRights::READ);
        if (!frame) return 0;

        return static_cast<std::uint64_t>(frame->size());
    }
    
    return static_cast<std::uint64_t>(-1);
}

void initSyscall() {
    std::uint64_t efer = oz::x86_64::readMSR(oz::x86_64::MSR_EFER);
    oz::x86_64::writeMSR(oz::x86_64::MSR_EFER, efer | (1ULL << 11) | 1);

    std::uint64_t star = (static_cast<std::uint64_t>(oz::x86_64::USER_CS32) << 48) | 
                         (static_cast<std::uint64_t>(oz::x86_64::KERNEL_CS) << 32);
    oz::x86_64::writeMSR(oz::x86_64::MSR_STAR, star);

    oz::x86_64::writeMSR(oz::x86_64::MSR_LSTAR, reinterpret_cast<std::uint64_t>(syscall_entry));
    oz::x86_64::writeMSR(oz::x86_64::MSR_FMASK, 0x200);
    oz::x86_64::writeMSR(oz::x86_64::MSR_KERNEL_GS_BASE, reinterpret_cast<std::uint64_t>(&g_syscall_context));
}

void setSyscallKernelStack(std::uint64_t rsp) {
    g_syscall_context.kernel_rsp = rsp;
    oz::x86_64::writeMSR(oz::x86_64::MSR_KERNEL_GS_BASE, reinterpret_cast<std::uint64_t>(&g_syscall_context));
}

__attribute__((naked)) void syscall_entry() {
    __asm__ volatile (
        "swapgs\n"
        "movq %rsp, %gs:0\n"
        "movq %gs:8, %rsp\n"
        
        "pushq %rbp\n"

        "pushq %rcx\n"
        "pushq %r11\n"
        "pushq %r9\n"
        "pushq %r8\n"
        "pushq %r10\n"
        "pushq %rdx\n"
        "pushq %rsi\n"
        "pushq %rdi\n"
        
        "movq %rsp, %rbp\n"
        "andq $-16, %rsp\n"
        
        "movq %r10, %rcx\n"
        "movq %r8, %r8\n"
        "movq %r9, %r9\n"
        "pushq $0\n" 
        "pushq %rax\n"

        "call handleSyscall\n"

        "movq %rbp, %rsp\n"
        
        "popq %rdi\n"
        "popq %rsi\n"
        "popq %rdx\n"
        "popq %r10\n"
        "popq %r8\n"
        "popq %r9\n"
        "popq %r11\n"
        "popq %rcx\n"
        
        "popq %rbp\n"

        "movq %gs:0, %rsp\n"
        "swapgs\n"
        "sysretq\n"
    );
}

} // namespace oz
