#include "hardware/x86_64.hpp"

#include <cstdint>
#include <cstddef>

#include "memory/Address.hpp"
#include "utils/utils.hpp"
#include "core/IRQDispatch.hpp"
#include "core/IRQHandler.hpp"
#include "core/Thread.hpp"
#include "core/Syscall.hpp"
#include "core/Channel.hpp"
#include "core/SavedInterruptFrame.hpp"
#include "core/Scheduler.hpp"
#include "utils/utils.hpp"

namespace {
    alignas(16) std::uint64_t gdt[8] = {
        0x0000000000000000, /* 0: NULL descriptor */
        0x00af9a000000ffff, /* 1: Kernel Code 64-bit */
        0x00cf93000000ffff, /* 2: Kernel Data */
        0x00cffa000000ffff, /* 3: User Code 32-bit (for sysret) */
        0x00cff2000000ffff, /* 4: User Data */
        0x00affa000000ffff, /* 5: User Code 64-bit */
        0x0000000000000000, /* 6: TSS Low */
        0x0000000000000000  /* 7: TSS High */
    };
    oz::x86_64::TaskStateSegment tss = {};
}

void oz::x86_64::initGDTR() {
    std::uint64_t tss_addr = reinterpret_cast<std::uint64_t>(&tss);
    std::uint64_t base_low = tss_addr & 0xffff;
    std::uint64_t base_mid = (tss_addr >> 16) & 0xff;
    std::uint64_t base_high = (tss_addr >> 24) & 0xff;
    std::uint64_t base_upper32 = tss_addr >> 32;

    gdt[6] = 0x0000890000000067 | (base_low << 16) | (base_mid << 32) | (base_high << 56);
    gdt[7] = base_upper32;
    tss.iomap_base = sizeof(tss);

    // GDTR requires 10 bytes: 2 bytes limit, 8 bytes base
    #pragma pack(push, 1)
    struct {
        std::uint16_t limit;
        std::uint64_t base;
    } gdtr;
    #pragma pack(pop)
    
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = reinterpret_cast<std::uint64_t>(gdt);

    __asm__ volatile("lgdt %[gdtr]" ::[gdtr] "m"(gdtr));

    __asm__ volatile(
        "movw %[ds], %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n" ::
            [ds] "i" (KERNEL_DS)
            : "%ax");

    std::uint64_t dummy;
    __asm__ volatile(
        "pushq %[selector]\n"
        "leaq 1f(%%rip), %[dummy]\n"
        "pushq %[dummy]\n"
        "lretq\n"
        "1:"
        : [dummy] "=r"(dummy)
        : [selector] "i"(KERNEL_CS));

    __asm__ volatile("ltr %%ax" :: "a"(TSS_SELECTOR));
}

void oz::x86_64::setKernelStack(std::uint64_t rsp) {
    tss.rsp0 = rsp;
}

namespace{
    struct InterruptDescriptor {
        std::uint16_t offset_00_15;
        std::uint16_t segment_selector;
        std::uint16_t flags;
        std::uint16_t offset_16_31;
        std::uint32_t offset_32_63;
        std::uint32_t reserved;
    };

    __attribute__((interrupt))
    void defaultHandler(oz::x86_64::ExceptionFrame* frame){
        (void)frame;
        oz::x86_64::notifyEndOfInterrupt();
    }

    void dumpExceptionInfo(const char* name, oz::x86_64::ExceptionFrame* frame, std::uint64_t error_code = 0) {
        char hexStr[17];
        dprint("\n\nEXCEPTION: ");
        dprint(name);
        dprint("\nRIP: 0x");
        oz::utils::to_hex(frame->rip, hexStr);
        dprint(hexStr);
        dprint("\nCS: 0x");
        oz::utils::to_hex(frame->cs, hexStr);
        dprint(hexStr);
        dprint("\nRFLAGS: 0x");
        oz::utils::to_hex(frame->rflags, hexStr);
        dprint(hexStr);
        dprint("\nRSP: 0x");
        oz::utils::to_hex(frame->rsp, hexStr);
        dprint(hexStr);
        dprint("\nSS: 0x");
        oz::utils::to_hex(frame->ss, hexStr);
        dprint(hexStr);
        dprint("\nError Code: 0x");
        oz::utils::to_hex(error_code, hexStr);
        dprint(hexStr);
        dprint("\nHALT.\n");
        while(true) { __asm__ volatile("hlt"); }
    }

    __attribute__((interrupt))
    void int00Handler(oz::x86_64::ExceptionFrame* frame) { dumpExceptionInfo("#DE Divide Error", frame); }

    __attribute__((interrupt))
    void int06Handler(oz::x86_64::ExceptionFrame* frame) { dumpExceptionInfo("#UD Invalid Opcode", frame); }

    __attribute__((interrupt))
    void int08Handler(oz::x86_64::ExceptionFrame* frame, std::uint64_t error_code) { dumpExceptionInfo("#DF Double Fault", frame, error_code); }

    __attribute__((interrupt))
    void int13Handler(oz::x86_64::ExceptionFrame* frame, std::uint64_t error_code) { dumpExceptionInfo("#GP General Protection", frame, error_code); }

    extern "C" void pageFaultHandler(std::uint64_t* regs) {
        using namespace oz;
        std::uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        std::uint64_t error_code = regs[15];
        std::uint64_t rip = regs[16];
        std::uint64_t cs  = regs[17];
        std::uint64_t rsp = regs[19];

        // If page fault from user space
        if ((cs & 3) != 0 && oz::g_get_current_thread_func) {
            Thread* current = oz::g_get_current_thread_func();
            if (current && current->fault_ep) {
                ChannelEndpoint* ep = current->fault_ep;
                ChannelEndpoint* peer = ep->get_peer();
                if (peer && !peer->is_peer_closed()) {
                    InternalIPCMessage msg{};
                    msg.user_msg.data[0] = cr2;
                    msg.user_msg.data[1] = error_code;
                    msg.user_msg.data[2] = rip;
                    msg.user_msg.data[3] = rsp;
                    msg.user_msg.has_cap = false;

                    Thread* pager = peer->pop_recv_waiter();
                    if (pager) {
                        if (pager->recv_msg_buf) {
                            *pager->recv_msg_buf = msg.user_msg;
                        } else {
                            peer->set_rendezvous_msg(msg);
                        }
                        pager->reply_target = current;
                        current->pending_server = pager;

                        // Hand-off baton from current to pager
                        Baton* b = current->get_active_baton();
                        if (current->borrowed_baton == b) {
                            current->borrowed_baton = nullptr;
                        }
                        pager->borrowed_baton = b;
                        if (b) {
                            b->thread = pager;
                            b->is_active = true;
                        }
                        current->baton = current->get_active_baton();
                        pager->baton = pager->get_active_baton();

                        current->state = ThreadState::BlockedOnReply;
                        current->is_blocked = true;

                        pager->state = ThreadState::Ready;
                        pager->is_blocked = false;
                        if (oz::g_unblock_thread_func) {
                            oz::g_unblock_thread_func(pager);
                        }

                        std::uint64_t now = oz::x86_64::rdtsc();
                        if (oz::g_scheduler_ptr) {
                            oz::g_scheduler_ptr->performContextSwitch(current, pager, now);
                        }
                        return; // Resumed after page was mapped and reply sent!
                    } else {
                        // Pager not yet waiting on recv
                        ep->set_rendezvous_msg(msg);
                        current->state = ThreadState::BlockedOnCall;
                        current->is_blocked = true;
                        ep->add_send_waiter(current);

                        if (peer->get_bound_notification()) {
                            peer->get_bound_notification()->signal(peer->get_bound_badge());
                        }

                        if (oz::g_block_thread_func) {
                            oz::g_block_thread_func();
                        }
                        return;
                    }
                }
            }

            // fault_ep is unconfigured or pager dead -> terminate thread and switch to root with SchedReason::FAULT
            if (current) {
                dprint("[Kernel] Unhandled user #PF without fault_ep: CR2=0x");
                char hexBuf[17];
                oz::utils::to_hex(cr2, hexBuf);
                dprint(hexBuf);
                dprint(" RIP=0x");
                oz::utils::to_hex(rip, hexBuf);
                dprint(hexBuf);
                dprint("\n\r");
                current->state = ThreadState::Dead;
                current->is_dead = true;
                ExecutionContext* caller = nullptr;
                if (current->baton) {
                    if (oz::g_scheduler_ptr) {
                        cartesianPopLeaf(oz::g_scheduler_ptr->cartesian_root, current->baton);
                    }
                    caller = current->baton->caller_context;
                    Baton* caller_b = current->baton->caller_baton;
                    current->baton->caller_baton = nullptr;
                    if (caller_b && oz::g_scheduler_ptr) {
                        caller_b->callee_baton = nullptr;
                        caller_b->state = BatonState::RUNNING;
                        cartesianPushDown(caller_b);
                        oz::g_scheduler_ptr->running_baton = caller_b;
                        if (!caller) caller = caller_b->thread;
                    }
                    current->baton->state = BatonState::BLOCKED;
                    current->baton->caller_context = nullptr;
                    current->baton->thread = nullptr;
                    current->baton->is_active = false;
                    current->baton = nullptr;
                }
                if (caller && oz::g_scheduler_ptr) {
                    caller->sched_reason = SchedReason::FAULT;
                    caller->preempted_irq_vector = 14; // Vector 14: #PF
                    std::uint64_t now = oz::x86_64::rdtsc();
                    oz::g_scheduler_ptr->performContextSwitch(current, caller, now);
                    while (1) { __asm__ volatile("cli; hlt"); }
                }
            }
        }

        // Unhandled page fault: dump and halt
        char hexStr[17];
        dprint("\nCR2: 0x");
        oz::utils::to_hex(cr2, hexStr);
        dprint(hexStr);
        dprint("\nRIP: 0x");
        oz::utils::to_hex(rip, hexStr);
        dprint(hexStr);
        dprint("\nError Code: 0x");
        oz::utils::to_hex(error_code, hexStr);
        dprint(hexStr);
        dprint("\nUnhandled Page Fault! System halted.\n\r");
        while (1) {
            __asm__ volatile("cli; hlt");
        }
    }

    extern "C" __attribute__((naked)) void page_fault_entry() {
        __asm__ volatile(
            // Hardware has pushed: SS, RSP, RFLAGS, CS, RIP, ERROR_CODE
            "pushq %rax\n"
            "pushq %rcx\n"
            "pushq %rdx\n"
            "pushq %rbx\n"
            "pushq %rbp\n"
            "pushq %rsi\n"
            "pushq %rdi\n"
            "pushq %r8\n"
            "pushq %r9\n"
            "pushq %r10\n"
            "pushq %r11\n"
            "pushq %r12\n"
            "pushq %r13\n"
            "pushq %r14\n"
            "pushq %r15\n"

            "movq %rsp, %rdi\n"
            "subq $8, %rsp\n"
            "call pageFaultHandler\n"
            "addq $8, %rsp\n"

            "popq %r15\n"
            "popq %r14\n"
            "popq %r13\n"
            "popq %r12\n"
            "popq %r11\n"
            "popq %r10\n"
            "popq %r9\n"
            "popq %r8\n"
            "popq %rdi\n"
            "popq %rsi\n"
            "popq %rbp\n"
            "popq %rbx\n"
            "popq %rdx\n"
            "popq %rcx\n"
            "popq %rax\n"

            // Pop error code
            "addq $8, %rsp\n"
            "iretq\n"
        );
    }

    InterruptDescriptor idt[oz::x86_64::MAX_INTERRUPTS_COUNT];

}


void oz::x86_64::setInterruptDescriptor(std::uint8_t index, void* handler){
    idt[index].offset_00_15 = reinterpret_cast<std::uint64_t>(handler) & 0xffff;
    idt[index].segment_selector = 8;
    idt[index].flags = 0x8e00;
    idt[index].offset_16_31 = (reinterpret_cast<std::uint64_t>(handler) >> 16) & 0xffff;
    idt[index].offset_32_63 = (reinterpret_cast<std::uint64_t>(handler) >> 32) & 0xffffffff;
    idt[index].reserved = 0;
}

void oz::x86_64::initIDTR() {
    for(std::size_t i = 0; i < MAX_INTERRUPTS_COUNT; i++){
        setInterruptDescriptor(i, reinterpret_cast<void*>(defaultHandler));
    }

    setInterruptDescriptor(0, reinterpret_cast<void*>(int00Handler));
    setInterruptDescriptor(6, reinterpret_cast<void*>(int06Handler));
    setInterruptDescriptor(8, reinterpret_cast<void*>(int08Handler));
    setInterruptDescriptor(13, reinterpret_cast<void*>(int13Handler));
    setInterruptDescriptor(14, reinterpret_cast<void*>(page_fault_entry));

    volatile std::uint64_t idtr[2];
    idtr[0] = (reinterpret_cast<std::uint64_t>(&idt) << 16) | (sizeof(idt) - 1);
    idtr[1] = (reinterpret_cast<std::uint64_t>(&idt) >> 48);
    __asm__ volatile("lidt %[idtr]" ::[idtr] "m"(idtr));
}

namespace {
    static void handleIRQ(std::uint8_t vector) {
        auto* handler = oz::irq::getHandler(vector);
        if (!handler) {
            oz::x86_64::notifyEndOfInterrupt();
            return;
        }

        // 1. すでに Mask 中 (ハンドラ走行中) に同一割り込みが再到着した場合: Pending として記録
        if (handler->isMasked()) {
            handler->setPending(true);
            oz::x86_64::notifyEndOfInterrupt();
            return;
        }

        // 2. ハードウェアラインの Mask と EOI 発行
        handler->mask();
        oz::x86_64::notifyEndOfInterrupt();

        oz::Thread* handler_thread = handler->handler_thread;
        oz::ExecutionContext* current = oz::g_scheduler_ptr ? oz::g_scheduler_ptr->getCurrentContext() : nullptr;

        if (!handler_thread) {
            // レガシー通知フォールバック
            handler->onInterrupt();
            return;
        }

        oz::Baton* irq_baton = handler->assigned_irq_baton;
        if (!irq_baton) {
            return; // バトン未登録
        }

        // ★ 3. バトンの残量検査 (明示補充モデル: 予算枯渇時のセーフティネット)
        if (irq_baton->remaining == 0) {
            handler->setPending(true);
            if (handler_thread->bound_notification) {
                handler_thread->bound_notification->signal(
                    handler_thread->sched_badge ? handler_thread->sched_badge : (1ULL << 62));
            }
            return;
        }

        // ★ 4. ワンショット・アンバインド: 発火と同時に待機登録を消費・解除
        handler->handler_thread = nullptr;
        handler_thread->bound_irq_handler = nullptr;

        // 5. 現在の LAPIC 残りカウンタと entry_tsc の取得
        std::uint32_t rem_ticks = oz::x86_64::getLAPICCurrentCount();
        std::uint64_t entry_tsc = oz::x86_64::rdtsc();
        std::uint64_t ticks_per_us = oz::x86_64::getLAPICTicksPerUs();
        if (ticks_per_us == 0) ticks_per_us = 1;

        // 6. ハンドラの退避フレーム (saved_frame) に被割り込みコンテキストを退避
        handler_thread->saved_frame.interrupted_context = current;
        handler_thread->saved_frame.lapic_remaining_ticks = rem_ticks;
        std::uint64_t init_ticks = irq_baton->remaining * ticks_per_us;
        if (init_ticks > 0xFFFFFFFFULL) init_ticks = 0xFFFFFFFFULL;
        handler_thread->saved_frame.lapic_initial_ticks = static_cast<std::uint32_t>(init_ticks);
        handler_thread->saved_frame.entry_tsc = entry_tsc;
        handler_thread->saved_frame.irq_vector = vector;

        // ネストスタックにプッシュ
        handler_thread->next_nested_irq = oz::g_current_irq_thread;
        oz::g_current_irq_thread = handler_thread;

        // 7. irq_baton の借用バインドと LAPIC タイマーセット
        handler_thread->borrowed_baton = irq_baton;
        handler_thread->baton = handler_thread->get_active_baton();
        irq_baton->thread = handler_thread;
        irq_baton->state = oz::BatonState::RUNNING;

        oz::x86_64::setLAPICOneShot(irq_baton->remaining, oz::x86_64::APIC_TIMER_VECTOR);

        // 8. ハンドラを起こして専用割り込みスイッチ
        handler_thread->is_blocked = false;
        handler_thread->state = oz::ThreadState::Ready;

        std::uint64_t now = oz::x86_64::rdtsc();
        if (oz::g_scheduler_ptr) {
            oz::g_scheduler_ptr->performIRQSwitchTo(handler_thread, now);
        }
    }

    extern "C" void timerInterruptHandler() {
        oz::x86_64::notifyEndOfInterrupt();
        auto tick_fn = oz::irq::getTimerTickFn();
        if (tick_fn) {
            tick_fn();
        }
    }

    extern "C" __attribute__((naked)) void timer_interrupt_entry() {
        __asm__ volatile(
            // Hardware has pushed: SS, RSP, RFLAGS, CS, RIP
            "pushq %rax\n"
            "pushq %rcx\n"
            "pushq %rdx\n"
            "pushq %rbx\n"
            "pushq %rbp\n"
            "pushq %rsi\n"
            "pushq %rdi\n"
            "pushq %r8\n"
            "pushq %r9\n"
            "pushq %r10\n"
            "pushq %r11\n"
            "pushq %r12\n"
            "pushq %r13\n"
            "pushq %r14\n"
            "pushq %r15\n"

            "call timerInterruptHandler\n"

            "popq %r15\n"
            "popq %r14\n"
            "popq %r13\n"
            "popq %r12\n"
            "popq %r11\n"
            "popq %r10\n"
            "popq %r9\n"
            "popq %r8\n"
            "popq %rdi\n"
            "popq %rsi\n"
            "popq %rbp\n"
            "popq %rbx\n"
            "popq %rdx\n"
            "popq %rcx\n"
            "popq %rax\n"

            "iretq\n"
        );
    }

#define IRQ_STUB(N) \
    __attribute__((interrupt)) \
    void irqStub##N(oz::x86_64::ExceptionFrame* frame) { \
        handleIRQ(0x##N); \
    }

    IRQ_STUB(30) IRQ_STUB(31) IRQ_STUB(32) IRQ_STUB(33)
    IRQ_STUB(34) IRQ_STUB(35) IRQ_STUB(36) IRQ_STUB(37)
    IRQ_STUB(38) IRQ_STUB(39) IRQ_STUB(3a) IRQ_STUB(3b)
    IRQ_STUB(3c) IRQ_STUB(3d) IRQ_STUB(3e) IRQ_STUB(3f)
    IRQ_STUB(40) IRQ_STUB(41) IRQ_STUB(42) IRQ_STUB(43)
    IRQ_STUB(44) IRQ_STUB(45) IRQ_STUB(46) IRQ_STUB(47)
    IRQ_STUB(48) IRQ_STUB(49) IRQ_STUB(4a) IRQ_STUB(4b)
    IRQ_STUB(4c) IRQ_STUB(4d) IRQ_STUB(4e) IRQ_STUB(4f)
}

void oz::x86_64::initIRQStubs() {
    setInterruptDescriptor(APIC_TIMER_VECTOR, reinterpret_cast<void*>(timer_interrupt_entry));
#define SET_STUB(N) setInterruptDescriptor(0x##N, reinterpret_cast<void*>(irqStub##N))
    SET_STUB(30); SET_STUB(31); SET_STUB(32); SET_STUB(33);
    SET_STUB(34); SET_STUB(35); SET_STUB(36); SET_STUB(37);
    SET_STUB(38); SET_STUB(39); SET_STUB(3a); SET_STUB(3b);
    SET_STUB(3c); SET_STUB(3d); SET_STUB(3e); SET_STUB(3f);
    SET_STUB(40); SET_STUB(41); SET_STUB(42); SET_STUB(43);
    SET_STUB(44); SET_STUB(45); SET_STUB(46); SET_STUB(47);
    SET_STUB(48); SET_STUB(49); SET_STUB(4a); SET_STUB(4b);
    SET_STUB(4c); SET_STUB(4d); SET_STUB(4e); SET_STUB(4f);
#undef SET_STUB
}

constexpr std::uint64_t END_OF_INTERRUPT_REGISTER_ADDR = oz::DIRECT_MAP_OFFSET + 0xfee000b0ULL;
__attribute__((no_caller_saved_registers))
void oz::x86_64::notifyEndOfInterrupt() {
    *reinterpret_cast<volatile std::uint32_t*>(END_OF_INTERRUPT_REGISTER_ADDR) = 0;
}

void oz::x86_64::setPageMap(const void* map) {
    volatile std::uint64_t pBuf = reinterpret_cast<std::uint64_t>(map);
    __asm__ volatile(
        "movq %0, %%cr3"
        ::"r"(pBuf)
    );
}

void oz::x86_64::enableSSE() {
    std::uint64_t dum[2];
    __asm__ volatile(
        "movq %%cr0, %%rax\n"
        "andq $-5, %%rax\n"//CR0.EM(bit2)をクリア
        "orq $2, %%rax\n"//CR0.MP(bit1)をセット
        "movq %%rax, %%cr0\n"
        "movq %%cr4, %%rax\n"
        "orq $1536, %%rax\n"//CR4.OSFXSR(bit9)をセット
        //CR4.OSXMMEXCPT(bit10)をセット
        "movq %%rax, %%cr4\n"
        : [dummy]"=m"(dum)
        :
        :"%rax", "%xmm0"
    );
}

std::uint8_t oz::x86_64::readIO8(std::uint16_t addr) {
    std::uint8_t value;
    __asm__ volatile(
        "inb %%dx, %%al\n"
        : "=a"(value) : "d"(addr)
    );
    return value;
}

std::uint16_t oz::x86_64::readIO16(std::uint16_t addr) {
    std::uint16_t value;
    __asm__ volatile(
        "inw %%dx, %%ax\n"
        : "=a"(value) : "d"(addr)
    );
    return value;
}

std::uint32_t oz::x86_64::readIO32(std::uint16_t addr) {
    std::uint32_t value;
    __asm__ volatile(
        "inl %%dx, %%eax\n"
        : "=a"(value) : "d"(addr)
    );
    return value;
}

void oz::x86_64::writeIO8(std::uint16_t addr, std::uint8_t value) {
    __asm__ volatile(
        "outb %[value], %[addr]"
        :: [value]"a"(value), [addr]"d"(addr)
    );
}

void oz::x86_64::writeIO16(std::uint16_t addr, std::uint16_t value) {
    __asm__ volatile(
        "outw %[value], %[addr]"
        :: [value]"a"(value), [addr]"d"(addr)
    );
}

void oz::x86_64::writeIO32(std::uint16_t addr, std::uint32_t value) {
    __asm__ volatile(
        "outl %[value], %[addr]"
        :: [value]"a"(value), [addr]"d"(addr)
    );
}



std::uint8_t oz::x86_64::getLocalAPICID() {
    std::uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));
    return static_cast<std::uint8_t>(ebx >> 24);
}

static std::uint64_t s_lapic_ticks_per_us = 100;
static std::uint64_t s_tsc_ticks_per_us = 1000;

void oz::x86_64::calibrateTimers() {
    volatile std::uint32_t* divide_config = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee003e0ULL);
    *divide_config = 0x3; // Divide by 16

    volatile std::uint32_t* lvt_timer = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee00320ULL);
    *lvt_timer = (1 << 16) | APIC_TIMER_VECTOR; // Masked, One-shot mode

    volatile std::uint32_t* initial_count = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee00380ULL);
    volatile std::uint32_t* current_count = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee00390ULL);

    // Disable speaker, set gate low (Port 0x61 bit 0 & 1 = 0)
    std::uint8_t p61 = oz::x86_64::readIO8(0x61);
    oz::x86_64::writeIO8(0x61, static_cast<std::uint8_t>(p61 & 0xFC));

    // PIT Channel 2, mode 0 (interrupt on terminal count), binary, access LSB then MSB
    oz::x86_64::writeIO8(0x43, 0b10110000);
    std::uint16_t pit_count = 11932; // ~10ms (1193182 Hz / 100)
    oz::x86_64::writeIO8(0x42, static_cast<std::uint8_t>(pit_count & 0xFF));
    oz::x86_64::writeIO8(0x42, static_cast<std::uint8_t>((pit_count >> 8) & 0xFF));

    // Start LAPIC timer with max count
    *initial_count = 0xFFFFFFFF;
    std::uint64_t tsc_start = oz::x86_64::rdtsc();

    // Start PIT counter: gate high
    p61 = oz::x86_64::readIO8(0x61);
    oz::x86_64::writeIO8(0x61, static_cast<std::uint8_t>((p61 & 0xFC) | 0x01));

    // Wait until PIT counter reaches 0 (Port 0x61 bit 5 goes high)
    while ((oz::x86_64::readIO8(0x61) & 0x20) == 0) {
        __asm__ volatile("pause");
    }

    std::uint64_t tsc_end = oz::x86_64::rdtsc();
    std::uint32_t lapic_rem = *current_count;
    *initial_count = 0; // Stop LAPIC timer

    // Reset Port 0x61
    oz::x86_64::writeIO8(0x61, static_cast<std::uint8_t>(p61 & 0xFC));

    std::uint64_t lapic_ticks_in_10ms = 0xFFFFFFFFULL - lapic_rem;
    std::uint64_t tsc_ticks_in_10ms = tsc_end - tsc_start;

    s_lapic_ticks_per_us = lapic_ticks_in_10ms / 10000;
    if (s_lapic_ticks_per_us == 0) s_lapic_ticks_per_us = 1;

    s_tsc_ticks_per_us = tsc_ticks_in_10ms / 10000;
    if (s_tsc_ticks_per_us == 0) s_tsc_ticks_per_us = 1;

    dprint("[LAPIC] Calibrated lapic_ticks_per_us: ");
    char b1[20];
    oz::utils::to_hex(s_lapic_ticks_per_us, b1);
    dprint(b1);
    dprint(", tsc_ticks_per_us: ");
    oz::utils::to_hex(s_tsc_ticks_per_us, b1);
    dprint(b1);
    dprint("\n\r");
}

void oz::x86_64::setLAPICOneShot(std::uint64_t time_budget_us, std::uint8_t vector) {
    volatile std::uint32_t* lvt_timer = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee00320ULL);
    volatile std::uint32_t* initial_count = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee00380ULL);

    if (time_budget_us == 0) {
        *initial_count = 0;
        *lvt_timer = (1 << 16) | vector; // Masked
        return;
    }

    std::uint64_t ticks = time_budget_us * s_lapic_ticks_per_us;
    if (ticks > 0xFFFFFFFFULL) ticks = 0xFFFFFFFFULL;
    if (ticks == 0) ticks = 1;

    // Unmask, One-Shot mode (bits 17-18 = 0)
    *lvt_timer = vector;
    *initial_count = static_cast<std::uint32_t>(ticks);
}

void oz::x86_64::setLAPICRemainingTicks(std::uint32_t ticks, std::uint8_t vector) {
    volatile std::uint32_t* lvt_timer = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee00320ULL);
    volatile std::uint32_t* initial_count = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee00380ULL);

    if (ticks == 0) {
        *initial_count = 0;
        *lvt_timer = (1 << 16) | vector; // Masked
        return;
    }

    *lvt_timer = vector;
    *initial_count = ticks;
}

std::uint32_t oz::x86_64::getLAPICCurrentCount() {
    volatile std::uint32_t* current_count = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee00390ULL);
    return *current_count;
}

void oz::x86_64::sendLAPICIPI(std::uint8_t vector) {
    volatile std::uint32_t* icr_low = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee00300ULL);
    *icr_low = (vector & 0xFF) | (1 << 14) | (1 << 18);
}

static bool s_external_irq_masked = false;

void oz::x86_64::maskExternalInterrupts() {
    s_external_irq_masked = true;
    // Set LAPIC TPR to mask all interrupts with priority lower than APIC_TIMER_VECTOR (vector 32)
    // Priority class is vector >> 4. Vector 32 is class 2, external IRQs (33+) are class 2 or higher.
    // Setting TPR to 0xFF masks all external interrupts except NMI.
    // However we want vector 32 to fire, so TPR = 0x20 would allow vector 32 (if vector 32 is class 2, APIC TPR allows > TPR)
    // But TPR suppresses priority <= (TPR >> 4). So TPR = 0x20 suppresses vector 0-31, but allows 32.
    // To suppress 33-255 while allowing 32, since 32 and 33-47 might share priority class 2,
    // software gating via s_external_irq_masked is safest and portable.
}

void oz::x86_64::unmaskExternalInterrupts() {
    s_external_irq_masked = false;
}

bool oz::x86_64::isExternalInterruptsMasked() {
    return s_external_irq_masked;
}

void oz::x86_64::stopLAPICTimer() {
    volatile std::uint32_t* lvt_timer = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee00320ULL);
    volatile std::uint32_t* initial_count = reinterpret_cast<volatile std::uint32_t*>(oz::DIRECT_MAP_OFFSET + 0xfee00380ULL);
    *initial_count = 0;
    *lvt_timer = (1 << 16) | APIC_TIMER_VECTOR; // Masked
}

std::uint64_t oz::x86_64::getTSCTicksPerUs() {
    return s_tsc_ticks_per_us;
}

std::uint64_t oz::x86_64::getLAPICTicksPerUs() {
    return s_lapic_ticks_per_us;
}

void oz::x86_64::initAPICTimer(std::uint8_t interrupt_vector) {
    std::uintptr_t apic_base = oz::DIRECT_MAP_OFFSET + 0xfee00000ULL;
    volatile std::uint32_t* divide_config = reinterpret_cast<volatile std::uint32_t*>(apic_base + 0x3e0);

    // Set divide config to 16
    *divide_config = 0x3;

    // Calibrate timers via PIT
    calibrateTimers();

    // Default: stopped / masked until sys_donate_time sets it
    stopLAPICTimer();
}
