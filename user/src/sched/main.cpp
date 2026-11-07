#include "syscall.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"
#include "../common/utils/utils.hpp"

// User-space Scheduler Daemon (Ring 3)
// Architecture: Baton Scheduling Context & Radix-64 Multi-stage Notification Model
// Policy: Baton (Scheduling Context) management, priority, time accounting, and pure Notification wakeup.

static constexpr std::uint32_t CAP_CONSOLE_WRITE = 1;
static constexpr std::uint32_t CAP_SCHED_RECV    = 2;
static constexpr std::uint32_t CAP_SCHED_NOTIF   = 3; // Root Notification for waking up

static constexpr std::size_t MAX_GROUPS = 4;
static constexpr std::size_t BATONS_PER_GROUP = 64;
static constexpr std::size_t MAX_BATONS = MAX_GROUPS * BATONS_PER_GROUP;
static constexpr std::uint64_t BADGE_SCHED_IPC = 1ULL << 63;

static constexpr std::size_t MAX_FREE_BATONS = 128;
static std::uint32_t g_baton_pool[MAX_FREE_BATONS]{};
static std::size_t g_baton_pool_count = 0;

static constexpr std::size_t MAX_IRQ_HANDLERS = 8;
struct IrqHandlerControlBlock {
    bool in_use{false};
    std::uint32_t tcb_cap{oz::INVALID_CAP_ID};
    std::uint32_t baton_cap{oz::INVALID_CAP_ID};
    std::uint64_t budget_us{10000};
    std::uint64_t badge{0};
    std::size_t consecutive_timeouts{0};
};
static IrqHandlerControlBlock g_irq_handlers[MAX_IRQ_HANDLERS]{};

static std::uint32_t alloc_baton() {
    if (g_baton_pool_count > 0) {
        return g_baton_pool[--g_baton_pool_count];
    }
    return oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
}

static void free_baton(std::uint32_t baton_cap) {
    if (baton_cap == oz::INVALID_CAP_ID) return;
    if (g_baton_pool_count < MAX_FREE_BATONS) {
        g_baton_pool[g_baton_pool_count++] = baton_cap;
    } else {
        oz::sys_cap_delete(baton_cap);
    }
}

// Baton Control Block (Scheduling Context ledger)
struct BatonControlBlock {
    bool in_use{false};
    std::uint32_t baton_cap{oz::INVALID_CAP_ID};
    std::uint32_t priority{1};
    bool is_ready{true};
    std::uint64_t total_cycles{0};
    std::uint64_t time_slice_us{10000}; // 10ms default donation budget
    // Debug & tracking context (bound TCB, if any)
    std::uint32_t bound_tcb_cap{oz::INVALID_CAP_ID};
    std::uint64_t bound_thread_id{0};
};

static BatonControlBlock g_batons[MAX_BATONS]{};
static std::uint32_t g_group_notifs[MAX_GROUPS]{};
static std::uint64_t g_ready_bitmaps[MAX_GROUPS]{};
static std::size_t g_current_index = MAX_BATONS - 1;
static std::size_t g_active_count = 0;

static void print_dec(std::uint64_t val) {
    if (val == 0) {
        oz::sys_debug_print("0");
        return;
    }
    char buf[24];
    int pos = 0;
    char tmp[24];
    int tpos = 0;
    while (val > 0) {
        tmp[tpos++] = '0' + (val % 10);
        val /= 10;
    }
    while (tpos > 0) {
        buf[pos++] = tmp[--tpos];
    }
    buf[pos] = '\0';
    oz::sys_debug_print(buf);
}

static std::size_t add_baton(std::uint32_t baton_cap, std::uint32_t priority, std::uint64_t budget_us,
                             std::uint32_t bound_tcb_cap = oz::INVALID_CAP_ID) {
    if (baton_cap == oz::INVALID_CAP_ID) return MAX_BATONS;

    for (std::size_t i = 0; i < MAX_BATONS; ++i) {
        if (!g_batons[i].in_use) {
            std::size_t g = i / BATONS_PER_GROUP;
            std::size_t slot = i % BATONS_PER_GROUP;

            g_batons[i].in_use = true;
            g_batons[i].baton_cap = baton_cap;
            g_batons[i].priority = priority ? priority : 1;
            g_batons[i].time_slice_us = budget_us ? budget_us : 10000;
            g_batons[i].is_ready = true;
            g_batons[i].total_cycles = 0;
            g_batons[i].bound_tcb_cap = bound_tcb_cap;

            std::uint64_t tid = 0;
            if (bound_tcb_cap != oz::INVALID_CAP_ID) {
                oz::sys_tcb_get_id(bound_tcb_cap, &tid);
            }
            g_batons[i].bound_thread_id = tid;

            // Register Radix-64 group Notification with slot badge on the baton
            oz::sys_baton_bind_notification(baton_cap, g_group_notifs[g], 1ULL << slot);

            g_ready_bitmaps[g] |= (1ULL << slot);
            g_active_count++;

            oz::sys_debug_print("[Sched] Registered baton slot=");
            print_dec(i);
            oz::sys_debug_print(", baton_cap=");
            print_dec(baton_cap);
            if (tid != 0) {
                oz::sys_debug_print(", bound_tid=");
                print_dec(tid);
            }
            oz::sys_debug_print("\n\r");
            return i;
        }
    }
    return MAX_BATONS;
}

static void remove_baton_by_slot(std::size_t i) {
    if (i >= MAX_BATONS || !g_batons[i].in_use) return;

    std::size_t g = i / BATONS_PER_GROUP;
    std::size_t slot = i % BATONS_PER_GROUP;

    g_ready_bitmaps[g] &= ~(1ULL << slot);

    if (g_batons[i].bound_tcb_cap != oz::INVALID_CAP_ID) {
        oz::sys_tcb_bind_baton(g_batons[i].bound_tcb_cap, oz::CAP_NULL);
    }
    oz::sys_baton_bind_notification(g_batons[i].baton_cap, oz::CAP_NULL, 0);
    free_baton(g_batons[i].baton_cap);

    g_batons[i].in_use = false;
    g_batons[i].baton_cap = oz::INVALID_CAP_ID;
    g_batons[i].bound_tcb_cap = oz::INVALID_CAP_ID;
    g_batons[i].bound_thread_id = 0;
    g_batons[i].is_ready = false;

    if (g_active_count > 0) g_active_count--;
}

static void remove_baton_by_cap(std::uint32_t cap) {
    for (std::size_t i = 0; i < MAX_BATONS; ++i) {
        if (g_batons[i].in_use && (g_batons[i].baton_cap == cap || g_batons[i].bound_tcb_cap == cap)) {
            remove_baton_by_slot(i);
            return;
        }
    }
}

static bool is_any_ready() {
    for (std::size_t g = 0; g < MAX_GROUPS; ++g) {
        if (g_ready_bitmaps[g] != 0) return true;
    }
    return false;
}

static std::size_t pick_next_ready_baton() {
    if (!is_any_ready()) return MAX_BATONS;

    for (std::size_t count = 0; count < MAX_BATONS; ++count) {
        g_current_index = (g_current_index + 1) % MAX_BATONS;
        std::size_t g = g_current_index / BATONS_PER_GROUP;
        std::size_t slot = g_current_index % BATONS_PER_GROUP;
        if ((g_ready_bitmaps[g] & (1ULL << slot)) != 0 && g_batons[g_current_index].in_use && g_batons[g_current_index].is_ready) {
            return g_current_index;
        }
    }
    return MAX_BATONS;
}

static void sched_ipc_worker() {
    oz::ReplyToken tok = oz::INVALID_REPLY_TOKEN;
    oz::IPCMessage req{};
    oz::IPCMessage resp{};
    std::uint32_t recv_slot = oz::g_slot_allocator.allocate_one();
    while (true) {
        tok = oz::sys_reply_recv(tok, CAP_SCHED_RECV, (tok != oz::INVALID_REPLY_TOKEN) ? &resp : nullptr, &req, recv_slot);
        if (tok == oz::INVALID_REPLY_TOKEN) {
            continue;
        }

        if (req.has_cap) {
            recv_slot = oz::g_slot_allocator.allocate_one();
        }

        auto cmd = static_cast<oz::ipc::SchedCmd>(req.data[0]);
        resp = oz::IPCMessage{};

        if (cmd == oz::ipc::SchedCmd::REGISTER_THREAD || cmd == oz::ipc::SchedCmd::REGISTER_BATON) {
            if (req.has_cap) {
                std::uint32_t cap = req.transferred_cap;
                std::uint32_t priority = static_cast<std::uint32_t>(req.data[1]);
                std::uint64_t budget = req.data[2] ? req.data[2] : 10000;

                // Allocate baton and bind to TCB (Legacy helper / Active thread setup)
                std::uint32_t baton = alloc_baton();
                if (baton != oz::INVALID_CAP_ID) {
                    oz::sys_tcb_bind_baton(cap, baton);
                    std::size_t idx = add_baton(baton, priority, budget, cap);
                    resp.data[0] = (idx < MAX_BATONS) ? 0 : 1;
                } else {
                    resp.data[0] = 1;
                }
            } else {
                resp.data[0] = 1;
            }
        } else if (cmd == oz::ipc::SchedCmd::CREATE_BATON) {
            std::uint32_t priority = static_cast<std::uint32_t>(req.data[1]);
            std::uint64_t budget = req.data[2] ? req.data[2] : 10000;
            std::uint32_t baton = alloc_baton();
            if (baton != oz::INVALID_CAP_ID) {
                std::size_t idx = add_baton(baton, priority, budget, oz::INVALID_CAP_ID);
                if (idx < MAX_BATONS) {
                    resp.data[0] = 0;
                    resp.transferred_cap = baton;
                    resp.has_cap = true;
                } else {
                    free_baton(baton);
                    resp.data[0] = 1;
                }
            } else {
                resp.data[0] = 1;
            }
        } else if (cmd == oz::ipc::SchedCmd::UNREGISTER_THREAD || cmd == oz::ipc::SchedCmd::UNREGISTER_BATON) {
            if (req.has_cap) {
                remove_baton_by_cap(req.transferred_cap);
            }
            resp.data[0] = 0;
        } else if (cmd == oz::ipc::SchedCmd::GET_SCHED_INFO) {
            resp.data[0] = 0;
            resp.data[1] = g_active_count;
        } else if (cmd == oz::ipc::SchedCmd::REGISTER_IRQ_HANDLER) {
            if (req.has_cap) {
                std::uint64_t budget_us = req.data[2] ? req.data[2] : 5000;
                std::uint32_t baton = alloc_baton();
                oz::sys_tcb_bind_baton(req.transferred_cap, baton);

                // Register handler block for replenishment tracking
                std::size_t h_idx = MAX_IRQ_HANDLERS;
                for (std::size_t i = 0; i < MAX_IRQ_HANDLERS; ++i) {
                    if (!g_irq_handlers[i].in_use) {
                        h_idx = i;
                        break;
                    }
                }
                if (h_idx < MAX_IRQ_HANDLERS) {
                    std::uint64_t badge = (1ULL << (50 + h_idx));
                    g_irq_handlers[h_idx].in_use = true;
                    g_irq_handlers[h_idx].tcb_cap = req.transferred_cap;
                    g_irq_handlers[h_idx].baton_cap = baton;
                    g_irq_handlers[h_idx].budget_us = budget_us;
                    g_irq_handlers[h_idx].badge = badge;
                    g_irq_handlers[h_idx].consecutive_timeouts = 0;

                    // Bind Root Notification and badge to thread for exhaustion notification
                    oz::sys_thread_set_notification(req.transferred_cap, CAP_SCHED_NOTIF, badge);
                }

                int inj_res = oz::sys_baton_inject(baton, budget_us);
                resp.data[0] = (inj_res == 0) ? 0 : 1;

                // Run handler once to let it initialize and enter reservation wait (sys_irq_wait)
                oz::sys_baton_switch(baton);
            } else {
                resp.data[0] = 1;
            }
        } else {
            resp.data[0] = 1;
        }
    }
}

static void process_woken_mask(std::uint64_t woken_mask) {
    // Check IRQ Handler Budget Exhaustion notifications (Section 4.1 & 6.2)
    for (std::size_t i = 0; i < MAX_IRQ_HANDLERS; ++i) {
        if (g_irq_handlers[i].in_use && (woken_mask & g_irq_handlers[i].badge)) {
            woken_mask &= ~g_irq_handlers[i].badge;

            oz::sys_debug_print("[Sched] IRQ Handler budget exhausted. Resuming handler on own_baton...\n\r");
            oz::sys_baton_inject(g_irq_handlers[i].baton_cap, g_irq_handlers[i].budget_us);

            // Resume interrupted IRQ handler from its interrupted rip
            oz::SchedResult res = oz::sys_baton_switch(g_irq_handlers[i].baton_cap);
            oz::sys_debug_print("[Sched] Replenishment baton_switch returned reason=");
            print_dec(static_cast<std::uint64_t>(res.reason));
            oz::sys_debug_print("\n\r");

            // Handle SYS_BATON_SWITCH_TIMEOUT return reason
            if (res.reason == oz::SYS_BATON_SWITCH_TIMEOUT) {
                g_irq_handlers[i].consecutive_timeouts++;
                oz::sys_debug_print("[Sched] WARNING: IRQ handler timed out again on re-execution! consecutive=");
                print_dec(g_irq_handlers[i].consecutive_timeouts);
                oz::sys_debug_print("\n\r");
                if (g_irq_handlers[i].consecutive_timeouts >= 3) {
                    oz::sys_debug_print("[Sched] ERROR: IRQ handler live-lock detected. Stopping injection.\n\r");
                }
            } else {
                g_irq_handlers[i].consecutive_timeouts = 0;
            }
        }
    }

    while (woken_mask != 0) {
        int g = __builtin_ctzll(woken_mask);
        woken_mask &= ~(1ULL << g);

        if (static_cast<std::size_t>(g) < MAX_GROUPS && g_group_notifs[g] != oz::INVALID_CAP_ID) {
            std::uint64_t woken_batons = 0;
            if (oz::sys_notification_poll(g_group_notifs[g], &woken_batons) == 0 && woken_batons != 0) {
                g_ready_bitmaps[g] |= woken_batons;
                while (woken_batons != 0) {
                    int slot = __builtin_ctzll(woken_batons);
                    woken_batons &= ~(1ULL << slot);
                    std::size_t idx = g * BATONS_PER_GROUP + slot;
                    if (idx < MAX_BATONS && g_batons[idx].in_use) {
                        g_batons[idx].is_ready = true;
                    }
                }
            }
        }
    }
}

extern "C" void oz_main(const oz::ProcessStartupInfo* info) {
    oz::LocalNamespace::init(info);
    oz::sys_debug_print("[Sched] User-space Scheduler Daemon started (Pure Baton Scheduling Context Model)!\n\r");

    // Initialize Radix-64 Child Notifications for groups
    for (std::size_t g = 0; g < MAX_GROUPS; ++g) {
        g_group_notifs[g] = oz::sys_notification_create(oz::CAP_UNTYPED);
        oz::sys_notification_bind_parent(g_group_notifs[g], CAP_SCHED_NOTIF, 1ULL << g);
    }

    // Pre-wire Core 5 TCBs from StartupInfo (MemMan, VFS, DrivMan, ProcMan)
    if (info && info->magic == oz::ProcessStartupInfo::MAGIC) {
        for (std::size_t i = 0; i < info->num_caps; ++i) {
            const auto& cap = info->caps[i];
            if (cap.role == oz::CapRole::CUSTOM &&
                cap.name[0] == 't' && cap.name[1] == 'c' && cap.name[2] == 'b' && cap.name[3] == ':') {
                std::uint32_t baton = alloc_baton();
                if (baton != oz::INVALID_CAP_ID) {
                    oz::sys_tcb_bind_baton(cap.slot, baton);
                    add_baton(baton, 1, 50000, cap.slot);
                    oz::sys_tcb_resume(cap.slot);
                    oz::sys_debug_print("[Sched] Pre-wired Core 5 TCB registered: ");
                    oz::sys_debug_print(cap.name);
                    oz::sys_debug_print("\n\r");
                }
            }
        }
    }

    // Spawn passive worker thread for CAP_SCHED_RECV (Thread-per-Endpoint model)
    std::uint32_t worker_tcb = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::TCB), 0);
    std::uint32_t worker_stack = oz::sys_frame_alloc(oz::CAP_UNTYPED, 2);
    static constexpr std::uint64_t WORKER_STACK_TOP = 0x700010000000ULL;
    oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, worker_stack, WORKER_STACK_TOP - 8192, 0x01, 8192);
    oz::sys_tcb_configure(worker_tcb, oz::CAP_SELF_CSPACE, oz::CAP_SELF_VSPACE,
                          reinterpret_cast<std::uint64_t>(sched_ipc_worker), WORKER_STACK_TOP - 8);
    oz::sys_tcb_resume(worker_tcb);

    // Initial hand-off to let worker reach sys_reply_recv(token=0) and unbind
    std::uint32_t worker_baton = alloc_baton();
    oz::sys_tcb_bind_baton(worker_tcb, worker_baton);
    oz::sys_baton_inject(worker_baton, 10000);
    oz::SchedResult w_res = oz::sys_baton_switch(worker_baton);
    if (w_res.reason == oz::SchedReason::ORPHANED || w_res.reason == oz::SchedReason::BLOCKED) {
        oz::sys_debug_print("[Sched] IPC worker initialized and passive.\n\r");
    }
    free_baton(worker_baton);

    while (true) {
        // 1. Batch collect woken up batons via O(K) Radix-64 Notification polling
        std::uint64_t woken_mask = 0;
        if (oz::sys_notification_poll(CAP_SCHED_NOTIF, &woken_mask) == 0 && woken_mask != 0) {
            process_woken_mask(woken_mask);
        }

        // 2. If no batons are ready, sleep in low-power idle (HLT) until an event arrives
        if (!is_any_ready()) {
            std::uint64_t idle_woken = 0;
            oz::sys_debug_print("[Sched] No ready batons! Calling notification_wait...\n\r");
            oz::sys_notification_wait(CAP_SCHED_NOTIF, &idle_woken);
            oz::sys_debug_print("[Sched] Woke from notification_wait, idle_woken=");
            print_dec(idle_woken);
            oz::sys_debug_print("\n\r");
            process_woken_mask(idle_woken);
            continue;
        }

        // 3. Pick next ready baton
        std::size_t next_idx = pick_next_ready_baton();
        if (next_idx >= MAX_BATONS) {
            continue;
        }

        BatonControlBlock& target = g_batons[next_idx];
        std::size_t g = next_idx / BATONS_PER_GROUP;
        std::size_t slot = next_idx % BATONS_PER_GROUP;

        // 4. Inject time and switch to target baton (Pure Baton Model)
        oz::sys_baton_inject(target.baton_cap, target.time_slice_us);
        oz::SchedResult res = oz::sys_baton_switch(target.baton_cap);

        oz::sys_debug_print("[Sched] baton_switch slot=");
        print_dec(next_idx);
        oz::sys_debug_print(", baton_cap=");
        print_dec(target.baton_cap);
        if (target.bound_thread_id != 0) {
            oz::sys_debug_print(", tid=");
            print_dec(target.bound_thread_id);
        }
        oz::sys_debug_print(" returned reason=");
        print_dec(static_cast<std::uint64_t>(res.reason));
        oz::sys_debug_print("\n\r");

        // 5. Account consumed cycles (unless PREEMPTED, where consumed_cycles contains irq_vector)
        if (res.reason != oz::SchedReason::PREEMPTED) {
            target.total_cycles += res.consumed_cycles;
        }

        // 6. Handle return reason
        switch (res.reason) {
            case oz::SchedReason::TIMEOUT:
                // Timeslice expired: still ready, round-robin continue (replenish next turn)
                break;
            case oz::SchedReason::YIELD:
                // Voluntarily yielded: still ready
                break;
            case oz::SchedReason::BLOCKED: {
                // Blocked on IPC/Notification: mark unready until notified
                target.is_ready = false;
                g_ready_bitmaps[g] &= ~(1ULL << slot);
                break;
            }
            case oz::SchedReason::PREEMPTED: {
                // Preempted by hardware interrupt handler timeout (Clean Unwind)
                // res.consumed_cycles contains the timed-out irq_vector!
                std::uint64_t irq_vector = res.consumed_cycles;
                oz::sys_debug_print("[Sched] Handled PREEMPTED: IRQ handler timed out on vector ");
                print_dec(irq_vector);
                oz::sys_debug_print("\n\r");
                // The interrupted baton target remains Ready in scheduler queue
                break;
            }
            case oz::SchedReason::ORPHANED: {
                // Target thread became passive or unlinked baton (e.g. sys_reply_recv with token=0)
                // The thread is still alive (passive worker); baton has no current thread to run.
                // Mark baton as unready, but do NOT free baton or destroy slot!
                oz::sys_debug_print("[Sched] Baton unlinked/orphaned on slot=");
                print_dec(next_idx);
                oz::sys_debug_print("\n\r");
                target.is_ready = false;
                g_ready_bitmaps[g] &= ~(1ULL << slot);
                break;
            }
            case oz::SchedReason::EXIT: {
                // Thread terminated: reclaim baton into free pool
                oz::sys_debug_print("[Sched] Reclaimed baton slot=");
                print_dec(next_idx);
                oz::sys_debug_print(" on thread EXIT\n\r");
                remove_baton_by_slot(next_idx);
                break;
            }
            case oz::SchedReason::FAULT: {
                // Thread died with unhandled CPU exception
                oz::sys_debug_print("[Sched] Handled FAULT: CPU exception on baton slot=");
                print_dec(next_idx);
                oz::sys_debug_print(", vector=");
                print_dec(res.consumed_cycles);
                oz::sys_debug_print("\n\r");
                remove_baton_by_slot(next_idx);
                break;
            }
            default: {
                if (static_cast<std::int64_t>(res.reason) < 0) {
                    target.is_ready = false;
                    g_ready_bitmaps[g] &= ~(1ULL << slot);
                }
                break;
            }
        }
    }
}
