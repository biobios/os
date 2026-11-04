#include "core/syscall/SyscallHandlers.hpp"

namespace oz {

static SyscallResult sys_pagetable_map_frame_handler(const SyscallArgs& args) {
    // sys_pagetable_map_frame(pt_cap, index, frame_cap, flags)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    Capability* pt_slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!pt_slot || pt_slot->type() != CapType::PageTable || !pt_slot->has_rights(CapRights::WRITE)) {
        return -1;
    }

    auto* pt_obj = pt_slot->pagetable_object();
    if (!pt_obj) return -1;

    if (args.rsi >= 512 || (pt_obj->level() == 4 && args.rsi >= 256)) {
        return -1;
    }

    Capability* frame_slot = cspace->getSlot(static_cast<CapID>(args.rdx));
    if (!frame_slot || frame_slot->type() != CapType::Frame || !frame_slot->has_rights(CapRights::READ)) {
        return -1;
    }

    int res = pt_obj->map_frame(args.rsi, frame_slot, args.r10);
    return res;
}

static SyscallResult sys_cap_unmap_handler(const SyscallArgs& args) {
    // sys_cap_unmap(mapped_cap)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    Capability* slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!slot || !slot->has_slot_flag(SLOT_FLAG_IS_MAPPED)) {
        return -1;
    }

    if (!unmap_capability_slot(slot)) {
        return -1;
    }
    return 0;
}

static SyscallResult sys_frame_phys_addr_handler(const SyscallArgs& args) {
    // frame_phys_addr(frame_cap)
    CSpace* cspace = get_cspace();
    if (!cspace) return 0;

    Capability* slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!slot || slot->type() != CapType::Frame || !slot->has_rights(CapRights::READ)) return 0;

    return static_cast<std::uint64_t>(slot->frame_phys());
}

static SyscallResult sys_pagetable_create_handler(const SyscallArgs& args) {
    // sys_pagetable_create(untyped_cap, frame_cap, level, target_cspace, target_slot)
    CSpace* current_cs = get_cspace();
    if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

    std::uint32_t level = static_cast<std::uint32_t>(args.rdx);
    if (level < 1 || level > 4) return SyscallResult(SYS_ERR_INVALID, 0);

    CapID target_cs_id = static_cast<CapID>(args.r10);
    CapID target_slot = static_cast<CapID>(args.r8);
    if (target_slot == 0 || target_slot >= CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
    if (is_reserved_system_slot(target_slot)) return SyscallResult(SYS_ERR_INVALID, 0);

    CSpace* target_cs = resolve_target_cspace(current_cs, target_cs_id);
    if (!target_cs) return SyscallResult(SYS_ERR_PERM, 0);

    Capability* target_slot_cap = target_cs->getSlot(target_slot);
    if (!target_slot_cap || !target_slot_cap->is_null()) {
        return SyscallResult(SYS_ERR_BUSY, 0);
    }

    Capability* ut_slot = current_cs->getSlot(static_cast<CapID>(args.rdi));
    if (!ut_slot || ut_slot->type() != CapType::UntypedMemory ||
        (!ut_slot->has_rights(CapRights::WRITE) && !ut_slot->has_rights(CapRights::DERIVE))) {
        return SyscallResult(SYS_ERR_PERM, 0);
    }
    if (ut_slot->untyped_is_device()) return SyscallResult(SYS_ERR_INVALID, 0);

    Capability* frame_slot = current_cs->getSlot(static_cast<CapID>(args.rsi));
    if (!PageTableObject::validate_backing_frame(frame_slot)) {
        return SyscallResult(SYS_ERR_INVALID, 0);
    }

    std::size_t saved_watermark = ut_slot->untyped_watermark();
    auto* pt_obj = untyped_allocate_object<PageTableObject>(ut_slot, static_cast<std::uint8_t>(level));
    if (!pt_obj) {
        ut_slot->set_untyped_watermark(saved_watermark);
        return SyscallResult(SYS_ERR_NO_MEMORY, 0);
    }

    pt_obj->bind_backing_frame(frame_slot, static_cast<std::uint8_t>(level));
    init_pagetable_object_slot(target_slot_cap, pt_obj, static_cast<std::uint8_t>(level));
    cdt_insert_child(ut_slot, target_slot_cap);
    return SyscallResult(0, 0);
}

static SyscallResult sys_untyped_revoke_handler(const SyscallArgs& args) {
    // untyped_revoke(untyped_cap)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    Capability* ut_slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!ut_slot || ut_slot->type() != CapType::UntypedMemory || !ut_slot->has_rights(CapRights::REVOKE)) {
        return -1;
    }

    cdt_revoke_subtree(ut_slot);

    Thread* current = get_current_thread();
    handleSelfDestructionIfNeeded(current);
    return 0;
}

static SyscallResult sys_pagetable_map_child_handler(const SyscallArgs& args) {
    // sys_pagetable_map(parent_pt_cap, index, child_pt_cap)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    Capability* parent_slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!parent_slot || (parent_slot->type() != CapType::PageTable && parent_slot->type() != CapType::VSpace) || !parent_slot->has_rights(CapRights::WRITE)) {
        return -1;
    }

    auto* parent_pt = parent_slot->pagetable_object();
    if (!parent_pt) return -1;

    if (args.rsi >= 512 || (parent_pt->level() == 4 && args.rsi >= 256)) {
        return -1;
    }

    Capability* child_slot = cspace->getSlot(static_cast<CapID>(args.rdx));
    if (!child_slot || (child_slot->type() != CapType::PageTable && child_slot->type() != CapType::VSpace)) {
        return -1;
    }

    int res = parent_pt->map_child_table(args.rsi, child_slot);
    return res;
}

static SyscallResult sys_frame_size_handler(const SyscallArgs& args) {
    // frame_size(frame_cap)
    CSpace* cspace = get_cspace();
    if (!cspace) return 0;

    Capability* frame_slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!frame_slot || frame_slot->type() != CapType::Frame || !frame_slot->has_rights(CapRights::READ)) {
        return 0;
    }

    return static_cast<std::uint64_t>(frame_slot->frame_size());
}

static SyscallResult handle_untyped_retype_bulk(
    CapID         ut_cap_id,
    ObjectType    target_type,
    std::uint64_t size_or_arg,
    CapID         target_cspace_id,
    CapID         start_slot,
    std::size_t   count
) {
    CSpace* current_cs = get_cspace();
    if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

    // Phase 1: 事前全数検証 (Dry-Run Validation)
    if (count == 0 || count > CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
    if (start_slot == 0 || start_slot >= CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
    if (count > CSpace::MAX_CAPS - start_slot) return SyscallResult(SYS_ERR_INVALID, 0);

    std::uint32_t end_slot = start_slot + count - 1;
    if (!(end_slot < CAP_RESERVED_SYSTEM_START || start_slot > CAP_RESERVED_SYSTEM_END)) {
        return SyscallResult(SYS_ERR_INVALID, 0);
    }

    if (target_type != ObjectType::Frame &&
        target_type != ObjectType::UntypedMemory &&
        target_type != ObjectType::TCB &&
        target_type != ObjectType::ChannelEndpoint &&
        target_type != ObjectType::Notification &&
        target_type != ObjectType::Baton &&
        target_type != ObjectType::CSpaceCap) {
        return SyscallResult(SYS_ERR_INVALID, 0);
    }

    if (target_type == ObjectType::ChannelEndpoint && (count % 2) != 0) {
        return SyscallResult(SYS_ERR_INVALID, 0);
    }

    if (target_type == ObjectType::UntypedMemory) {
        if (size_or_arg == 0 || (size_or_arg % 4096) != 0) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }
    } else if (target_type == ObjectType::Frame) {
        if (size_or_arg != 0 && size_or_arg != 4096) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }
    }

    Capability* ut_slot = current_cs->getSlot(ut_cap_id);
    if (!ut_slot || ut_slot->type() != CapType::UntypedMemory ||
        (!ut_slot->has_rights(CapRights::WRITE) && !ut_slot->has_rights(CapRights::DERIVE))) {
        return SyscallResult(SYS_ERR_PERM, 0);
    }

    if (ut_slot->untyped_is_device()) {
        if (target_type != ObjectType::Frame && target_type != ObjectType::UntypedMemory) {
            return SyscallResult(SYS_ERR_INVALID, 0);
        }
    }

    CSpace* target_cs = resolve_target_cspace(current_cs, target_cspace_id);
    if (!target_cs) {
        return SyscallResult(SYS_ERR_PERM, 0);
    }

    if (!target_cs->is_range_null(start_slot, count)) {
        return SyscallResult(SYS_ERR_BUSY, 0);
    }

    if (!untyped_can_allocate_bulk(ut_slot, target_type, size_or_arg, count)) {
        return SyscallResult(SYS_ERR_NO_MEMORY, 0);
    }

    // Phase 2: 一括確定 (No-fail Commit)
    if (target_type == ObjectType::Frame) {
        std::uintptr_t phys_base = 0;
        void* mem = untyped_allocate_bytes(ut_slot, count * 4096, 4096, phys_base);
        (void)mem;
        for (std::size_t i = 0; i < count; ++i) {
            Capability* s = target_cs->getSlot(start_slot + i);
            init_frame_slot(s, phys_base + i * 4096, CapRights::ALL, true);
            cdt_insert_child(ut_slot, s);
        }
    } else if (target_type == ObjectType::ChannelEndpoint) {
        for (std::size_t i = 0; i < count; i += 2) {
            ChannelEndpoint* end0 = untyped_allocate_object<ChannelEndpoint>(ut_slot, true);
            ChannelEndpoint* end1 = untyped_allocate_object<ChannelEndpoint>(ut_slot, true);
            end0->set_peer(end1);
            end1->set_peer(end0);

            Capability* s0 = target_cs->getSlot(start_slot + i);
            Capability* s1 = target_cs->getSlot(start_slot + i + 1);
            init_kernel_object_slot(s0, end0, CapRights::ALL);
            init_kernel_object_slot(s1, end1, CapRights::ALL);
            cdt_insert_child(ut_slot, s0);
            cdt_insert_child(ut_slot, s1);
        }
    } else if (target_type == ObjectType::CSpaceCap) {
        for (std::size_t i = 0; i < count; ++i) {
            std::uintptr_t cs_phys = 0;
            void* cs_mem = untyped_allocate_bytes(ut_slot, sizeof(CSpace), alignof(CSpace), cs_phys);
            CSpace* child_cspace = new (cs_mem) CSpace();
            CSpaceCap* cs_cap_obj = untyped_allocate_object<CSpaceCap>(ut_slot, child_cspace, false, true);

            Capability* s = target_cs->getSlot(start_slot + i);
            init_kernel_object_slot(s, cs_cap_obj, CapRights::ALL);
            cdt_insert_child(ut_slot, s);
        }
    } else if (target_type == ObjectType::UntypedMemory) {
        for (std::size_t i = 0; i < count; ++i) {
            std::uintptr_t sub_phys = 0;
            void* sub_mem = untyped_allocate_bytes(ut_slot, size_or_arg, 4096, sub_phys);
            (void)sub_mem;
            Capability* s = target_cs->getSlot(start_slot + i);
            init_untyped_slot(s, sub_phys, size_or_arg, ut_slot->untyped_is_device());
            cdt_insert_child(ut_slot, s);
        }
    } else if (target_type == ObjectType::TCB) {
        for (std::size_t i = 0; i < count; ++i) {
            TCB* tcb = untyped_retype_tcb(ut_slot);
            Capability* s = target_cs->getSlot(start_slot + i);
            init_kernel_object_slot(s, tcb, CapRights::ALL);
            cdt_insert_child(ut_slot, s);
        }
    } else if (target_type == ObjectType::Notification) {
        for (std::size_t i = 0; i < count; ++i) {
            Notification* notif = untyped_allocate_object<Notification>(ut_slot, true);
            Capability* s = target_cs->getSlot(start_slot + i);
            init_kernel_object_slot(s, notif, CapRights::ALL);
            cdt_insert_child(ut_slot, s);
        }
    } else if (target_type == ObjectType::Baton) {
        for (std::size_t i = 0; i < count; ++i) {
            Baton* baton = untyped_allocate_object<Baton>(ut_slot, true);
            Capability* s = target_cs->getSlot(start_slot + i);
            init_kernel_object_slot(s, baton, CapRights::ALL);
            cdt_insert_child(ut_slot, s);
        }
    }

    return SyscallResult(0, 0);
}

static SyscallResult sys_untyped_retype_bulk_handler(const SyscallArgs& args) {
    // sys_untyped_retype_bulk(ut_cap, type, size_or_arg, target_cspace, start_slot, count)
    return handle_untyped_retype_bulk(
        static_cast<CapID>(args.rdi),
        static_cast<ObjectType>(args.rsi),
        args.rdx,
        static_cast<CapID>(args.r10),
        static_cast<CapID>(args.r8),
        static_cast<std::size_t>(args.r9)
    );
}

SyscallResult handleSyscallMemory(const SyscallArgs& args) {
    switch (args.number) {
        case Syscall::PageTableMapFrame:
            return sys_pagetable_map_frame_handler(args);
        case Syscall::PageTableUnmap:
            return sys_cap_unmap_handler(args);
        case Syscall::FrameGetPhys:
            return sys_frame_phys_addr_handler(args);
        case Syscall::PageTableCreate:
            return sys_pagetable_create_handler(args);
        case Syscall::UntypedRetype:
            return sys_untyped_revoke_handler(args);
        case Syscall::PageTableMapChild:
            return sys_pagetable_map_child_handler(args);
        case Syscall::FrameGetSize:
            return sys_frame_size_handler(args);
        case Syscall::UntypedRetypeBulk:
            return sys_untyped_retype_bulk_handler(args);
        default:
            return SyscallResult(SYS_ERR_INVALID, 0);
    }
}

} // namespace oz
