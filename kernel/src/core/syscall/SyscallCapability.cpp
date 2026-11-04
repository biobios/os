#include "core/syscall/SyscallHandlers.hpp"

namespace oz {

static SyscallResult sys_cap_mint_handler(const SyscallArgs& args) {
    // cap_mint(target_cspace, target_slot, src_cap, new_rights, badge)
    CSpace* current_cs = get_cspace();
    if (!current_cs) return SyscallResult(SYS_ERR_INVALID, 0);

    CapID target_cs_id = static_cast<CapID>(args.rdi);
    CapID target_slot = static_cast<CapID>(args.rsi);
    CapID src_cap = static_cast<CapID>(args.rdx);
    CapRights new_rights = static_cast<CapRights>(args.r10);
    std::uint64_t badge = args.r8;

    if (target_slot == 0 || target_slot >= CSpace::MAX_CAPS) return SyscallResult(SYS_ERR_INVALID, 0);
    if (is_reserved_system_slot(target_slot)) return SyscallResult(SYS_ERR_INVALID, 0);

    CSpace* target_cs = resolve_target_cspace(current_cs, target_cs_id);
    if (!target_cs) return SyscallResult(SYS_ERR_PERM, 0);

    Capability* target_slot_cap = target_cs->getSlot(target_slot);
    if (!target_slot_cap || !target_slot_cap->is_null()) {
        return SyscallResult(SYS_ERR_BUSY, 0);
    }

    Capability* src_slot = current_cs->getSlot(src_cap);
    if (!src_slot || src_slot->is_null()) return SyscallResult(SYS_ERR_INVALID, 0);

    // Privilege escalation check
    if (new_rights == CapRights::ALL) {
        new_rights = src_slot->rights();
    } else if ((static_cast<std::uint32_t>(new_rights) & ~static_cast<std::uint32_t>(src_slot->rights())) != 0) {
        return SyscallResult(SYS_ERR_PERM, 0);
    }

    if (!cap_copy(target_cs, target_slot, current_cs, src_cap, new_rights, badge, badge != 0)) {
        return SyscallResult(SYS_ERR_INVALID, 0);
    }
    return SyscallResult(0, 0);
}

static SyscallResult sys_cap_delete_handler(const SyscallArgs& args) {
    // cap_delete(slot)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    Capability* slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!slot || slot->is_null()) return -1;

    cdt_remove_and_reparent(slot);

    Thread* current = get_current_thread();
    handleSelfDestructionIfNeeded(current);
    return 0;
}

static SyscallResult sys_cap_copy_handler(const SyscallArgs& args) {
    // cspace_cap_insert(target_cspace_cap, target_slot, src_cap_in_current_cspace, rights)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;

    CSpaceCap* target_cs = get_cap_typed<CSpaceCap>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::CSpaceCap, CapRights::WRITE);
    if (!target_cs || !target_cs->getCSpace()) return -1;

    CapID target_slot = static_cast<CapID>(args.rsi);
    CapID src_slot = static_cast<CapID>(args.rdx);
    CapRights new_rights = static_cast<CapRights>(args.r10);

    if (!cap_copy(target_cs->getCSpace(), target_slot, cspace, src_slot, new_rights)) {
        return -1;
    }
    return 0;
}

static SyscallResult sys_cap_move_handler(const SyscallArgs& args) {
    // cap_move(dest_slot, src_slot)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;
    CapID dest = static_cast<CapID>(args.rdi);
    CapID src = static_cast<CapID>(args.rsi);
    if (!cap_move(cspace, dest, src)) return -1;
    return 0;
}

static SyscallResult sys_cap_move_remote_handler(const SyscallArgs& args) {
    // cspace_cap_move(target_cspace_cap, target_slot, src_slot)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;
    CSpaceCap* target_cs = get_cap_typed<CSpaceCap>(
        cspace, static_cast<CapID>(args.rdi), ObjectType::CSpaceCap, CapRights::WRITE);
    if (!target_cs || !target_cs->getCSpace()) return -1;

    CapID target_slot = static_cast<CapID>(args.rsi);
    CapID src_slot = static_cast<CapID>(args.rdx);
    if (!cap_move(target_cs->getCSpace(), target_slot, cspace, src_slot)) return -1;
    return 0;
}

static SyscallResult sys_cap_derive_handler(const SyscallArgs& args) {
    // cap_copy(dest_slot, src_slot, rights)
    CSpace* cspace = get_cspace();
    if (!cspace) return -1;
    CapID dest = static_cast<CapID>(args.rdi);
    CapID src = static_cast<CapID>(args.rsi);
    CapRights rights = static_cast<CapRights>(args.rdx);
    if (!cap_copy(cspace, dest, src, rights)) return -1;
    return 0;
}

static SyscallResult sys_cap_revoke_handler(const SyscallArgs& args) {
    // sys_cap_revoke(cap_id)
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    if (!cspace->revoke(static_cast<CapID>(args.rdi))) {
        return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
    }
    Thread* current = get_current_thread();
    handleSelfDestructionIfNeeded(current);
    return SyscallResult(0, 0);
}

static SyscallResult sys_cap_identify_handler(const SyscallArgs& args) {
    // sys_cap_identify(cap_id, info_ptr)
    CSpace* cspace = get_cspace();
    if (!cspace) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);
    if (!is_user_buffer(args.rsi, sizeof(CapInfo))) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    Capability* slot = cspace->getSlot(static_cast<CapID>(args.rdi));
    if (!slot || slot->is_null()) return SyscallResult(static_cast<std::uint64_t>(SYS_ERR_INVALID), 0);

    CapInfo info{};
    info.type = static_cast<std::uint8_t>(slot->type());
    info.level = 0;
    info.is_mapped = slot->has_slot_flag(SLOT_FLAG_IS_MAPPED) ? 1 : 0;
    info.padding = 0;
    info.rights = static_cast<std::uint32_t>(slot->rights());
    info.size = 0;

    if (slot->type() == CapType::Frame) {
        info.size = 4096;
    } else if (slot->type() == CapType::PageTable || slot->type() == CapType::VSpace) {
        auto* pt = slot->pagetable_object();
        if (pt) info.level = pt->level();
        info.size = 4096;
    } else if (slot->type() == CapType::UntypedMemory) {
        info.size = slot->untyped_size();
    }

    utils::memcpy(reinterpret_cast<void*>(args.rsi), &info, sizeof(CapInfo));
    return SyscallResult(0, 0);
}

SyscallResult handleSyscallCapability(const SyscallArgs& args) {
    switch (args.number) {
        case Syscall::CapMint:
            return sys_cap_mint_handler(args);
        case Syscall::CapDelete:
            return sys_cap_delete_handler(args);
        case Syscall::CapCopy:
            return sys_cap_copy_handler(args);
        case Syscall::CapMove:
            return sys_cap_move_handler(args);
        case Syscall::CapMoveRemote:
            return sys_cap_move_remote_handler(args);
        case Syscall::CapDerive:
            return sys_cap_derive_handler(args);
        case Syscall::CapRevoke:
            return sys_cap_revoke_handler(args);
        case Syscall::CapIdentify:
            return sys_cap_identify_handler(args);
        default:
            return SyscallResult(SYS_ERR_INVALID, 0);
    }
}

} // namespace oz
