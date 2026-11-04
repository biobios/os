#pragma once

#include <cstdint>
#include <cstddef>

namespace oz {

class PageTableObject;
struct Capability;
void relocate_pagetable_links_on_move(Capability* dest, Capability* src);

enum class CapRights : std::uint32_t {
    NONE        = 0,

    // Common Rights (bit 0..15)
    DUPLICATE   = 1U << 0,
    TRANSFER    = 1U << 1,
    DERIVE      = 1U << 2,
    REVOKE      = 1U << 3,

    // Type Rights (bit 16..31)
    READ        = 1U << 16,
    RECEIVE     = 1U << 16,
    WAIT        = 1U << 16,
    CONFIGURE   = 1U << 16,
    SWITCH      = 1U << 16,
    IN          = 1U << 16,
    INSERT      = 1U << 16,

    WRITE       = 1U << 17,
    SEND        = 1U << 17,
    SIGNAL      = 1U << 17,
    CONTROL     = 1U << 17,
    OUT         = 1U << 17,
    ACK_WAIT    = 1U << 17,
    DELETE      = 1U << 17,

    EXEC        = 1U << 18,
    GRANT       = 1U << 18,
    BIND        = 1U << 18,
    SET_BUDGET  = 1U << 18,

    NOCACHE     = 1U << 19,
    GRANT_REPLY = 1U << 19,
    DEBUG       = 1U << 19,

    USE         = 1U << 4, // 互換用

    ALL         = 0xFFFFFFFFU,
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

enum class CapType : std::uint8_t {
    Null            = 0,
    // カテゴリ A: インライン型 / KernelObject 不要型
    UntypedMemory   = 1,
    IOPort          = 2,
    IRQBatonControl = 3,
    IRQHandler      = 4,
    Frame           = 5,
    PageTable       = 6,
    // カテゴリ B: KernelObject 参照型 (すべてデフォルト構築)
    ChannelEndpoint = 7,
    Notification    = 8,
    Baton           = 9,
    TCB             = 10,
    CSpaceCap       = 11,
    VSpace          = 12,
};

enum class ObjectType : std::uint8_t {
    Unknown         = 0,
    ChannelEndpoint = 1,
    Frame           = 2,
    UntypedMemory   = 3,
    VSpace          = 4,
    TCB             = 5,
    CSpaceCap       = 6,
    IRQHandler      = 7,
    IOPort          = 8,
    PageTable       = 9,
    Notification    = 10,
    Baton           = 11,
    IRQBatonControl = 12,
};

inline constexpr CapType to_cap_type(ObjectType t) {
    switch (t) {
        case ObjectType::UntypedMemory:   return CapType::UntypedMemory;
        case ObjectType::IOPort:          return CapType::IOPort;
        case ObjectType::IRQBatonControl: return CapType::IRQBatonControl;
        case ObjectType::IRQHandler:      return CapType::IRQHandler;
        case ObjectType::Frame:           return CapType::Frame;
        case ObjectType::PageTable:       return CapType::PageTable;
        case ObjectType::ChannelEndpoint: return CapType::ChannelEndpoint;
        case ObjectType::Notification:    return CapType::Notification;
        case ObjectType::Baton:           return CapType::Baton;
        case ObjectType::TCB:             return CapType::TCB;
        case ObjectType::CSpaceCap:       return CapType::CSpaceCap;
        case ObjectType::VSpace:          return CapType::VSpace;
        default:                          return CapType::Null;
    }
}

inline constexpr ObjectType to_object_type(CapType t) {
    switch (t) {
        case CapType::UntypedMemory:   return ObjectType::UntypedMemory;
        case CapType::IOPort:          return ObjectType::IOPort;
        case CapType::IRQBatonControl: return ObjectType::IRQBatonControl;
        case CapType::IRQHandler:      return ObjectType::IRQHandler;
        case CapType::Frame:           return ObjectType::Frame;
        case CapType::PageTable:       return ObjectType::PageTable;
        case CapType::ChannelEndpoint: return ObjectType::ChannelEndpoint;
        case CapType::Notification:    return ObjectType::Notification;
        case CapType::Baton:           return ObjectType::Baton;
        case CapType::TCB:             return ObjectType::TCB;
        case CapType::CSpaceCap:       return ObjectType::CSpaceCap;
        case CapType::VSpace:          return ObjectType::VSpace;
        default:                       return ObjectType::Unknown;
    }
}

inline constexpr CapType to_cap_type(std::uint32_t val) {
    if (val >= 1 && val <= 12) {
        return to_cap_type(static_cast<ObjectType>(val));
    }
    return CapType::Null;
}

constexpr std::uint8_t SLOT_FLAG_HAS_KERNEL_OBJECT = 1 << 0;
constexpr std::uint8_t SLOT_FLAG_IN_REVOKE         = 1 << 1;
constexpr std::uint8_t SLOT_FLAG_PENDING_DELETE    = 1 << 2;
constexpr std::uint8_t SLOT_FLAG_IS_MAPPED         = 1 << 3;
constexpr std::uint8_t SLOT_FLAG_PT_BACKING        = 1 << 4;
constexpr std::uint8_t SLOT_FLAG_ORIGINAL          = 1 << 5;

struct CapInfo {
    std::uint8_t  type{0};
    std::uint8_t  level{0};
    std::uint8_t  is_mapped{0};
    std::uint8_t  padding{0};
    std::uint32_t rights{0};
    std::uint64_t size{0};
};

class KernelObject {
protected:
    std::uint32_t ref_count_{1};
    ObjectType    obj_type_{ObjectType::Unknown};
    bool          is_untyped_backed_{true};
    bool          is_revoked_{false};

public:
    explicit KernelObject(ObjectType t = ObjectType::Unknown, bool untyped = true)
        : ref_count_(1), obj_type_(t), is_untyped_backed_(untyped), is_revoked_(false) {}

    virtual ~KernelObject() = default;

    ObjectType objectType() const { return obj_type_; }
    bool isUntypedBacked() const { return is_untyped_backed_; }
    void setUntypedBacked(bool untyped) { is_untyped_backed_ = untyped; }

    bool isRevoked() const { return is_revoked_; }
    void markRevoked() { is_revoked_ = true; }

    void add_ref() { ++ref_count_; }
    void release() {
        if (ref_count_ > 0 && --ref_count_ == 0) {
            destroy();
        }
    }
    std::uint32_t get_ref_count() const { return ref_count_; }

    virtual void destroy() {
        if (!is_untyped_backed_) {
            delete this;
        } else {
            this->~KernelObject();
        }
    }
};

struct alignas(64) Capability {
    std::uint64_t data[4]{0, 0, 0, 0};       // 32 bytes
    std::uint64_t flags{0};                  //  8 bytes
    Capability*   cdt_parent{nullptr};       //  8 bytes
    Capability*   cdt_next_child{nullptr};   //  8 bytes
    Capability*   cdt_next_sibling{nullptr}; //  8 bytes

    Capability() = default;
    Capability(const Capability&) = delete;
    Capability& operator=(const Capability&) = delete;
    Capability(Capability&&) = delete;
    Capability& operator=(Capability&&) = delete;
    ~Capability() = default;

    bool is_null() const { return type() == CapType::Null; }
    bool is_valid() const { return !is_null(); }

    CapType type() const { return static_cast<CapType>(flags & 0xFF); }
    void set_type(CapType t) {
        flags = (flags & ~0xFFULL) | static_cast<std::uint8_t>(t);
    }

    std::uint8_t slot_flags() const {
        return static_cast<std::uint8_t>((flags >> 8) & 0xFF);
    }
    bool has_slot_flag(std::uint8_t f) const {
        return (slot_flags() & f) != 0;
    }
    void set_slot_flag(std::uint8_t f) {
        flags |= (static_cast<std::uint64_t>(f) << 8);
    }
    void clear_slot_flag(std::uint8_t f) {
        flags &= ~(static_cast<std::uint64_t>(f) << 8);
    }

    std::uint16_t map_entry_index() const {
        return static_cast<std::uint16_t>((flags >> 16) & 0x1FFULL);
    }
    void set_map_entry_index(std::uint16_t idx) {
        flags = (flags & ~(0x1FFULL << 16)) | ((static_cast<std::uint64_t>(idx) & 0x1FFULL) << 16);
    }

    CapRights rights() const {
        return static_cast<CapRights>((flags >> 32) & 0xFFFFFFFF);
    }
    void set_rights(CapRights r) {
        flags = (flags & 0x00000000FFFFFFFFULL) | (static_cast<std::uint64_t>(r) << 32);
    }
    bool has_rights(CapRights r) const {
        return (rights() & r) == r;
    }
    bool can_duplicate() const {
        return is_valid() && has_rights(CapRights::DUPLICATE);
    }

    KernelObject* object() const {
        if (has_slot_flag(SLOT_FLAG_HAS_KERNEL_OBJECT)) {
            return reinterpret_cast<KernelObject*>(data[0]);
        }
        return nullptr;
    }
    std::uint64_t badge() const {
        return data[1];
    }
    void set_badge(std::uint64_t b) {
        data[1] = b;
    }

    // --- Type-specific inline data accessors ---
    // UntypedMemory: data[0]=phys_base, data[1]=size_bytes, data[2]=watermark, data[3]=is_device
    std::uintptr_t untyped_phys_base() const { return data[0]; }
    std::size_t untyped_size() const { return data[1]; }
    std::size_t untyped_watermark() const { return data[2]; }
    void set_untyped_watermark(std::size_t wm) { data[2] = wm; }
    bool untyped_is_device() const { return data[3] != 0; }

    // Frame: data[0]=phys_addr, data[1]=parent_pt / bound_pt, data[2]=map_next, data[3]=map_prev
    std::uintptr_t frame_phys() const { return data[0]; }
    std::size_t frame_size() const { return 4096; }

    // PageTable / VSpace: data[0]=self_pt, data[1]=parent_pt, data[2]=parent_map_next, data[3]=self_map_next
    PageTableObject* pagetable_object() const {
        if (type() == CapType::PageTable || type() == CapType::VSpace) {
            return reinterpret_cast<PageTableObject*>(data[0]);
        }
        return nullptr;
    }

    // IOPort: data[0]=base_port | (port_count << 32)
    std::uint16_t ioport_base() const { return static_cast<std::uint16_t>(data[0] & 0xFFFF); }
    std::uint32_t ioport_count() const { return static_cast<std::uint32_t>(data[0] >> 32); }

    // IRQHandler: data[0]=vector, data[1]=entry_ptr
    std::uint8_t irq_vector() const { return static_cast<std::uint8_t>(data[0] & 0xFF); }
    template <typename T = void>
    T* irq_handler_entry() const { return reinterpret_cast<T*>(data[1]); }

    void clear_without_release() {
        data[0] = 0;
        data[1] = 0;
        data[2] = 0;
        data[3] = 0;
        flags = 0;
        cdt_parent = nullptr;
        cdt_next_child = nullptr;
        cdt_next_sibling = nullptr;
    }

    void release_and_clear();
};
static_assert(sizeof(Capability) == 64, "Capability slot must be exactly 64 bytes");

// Type-safe Capability Data Layouts (32 bytes each)
struct FrameData {
    std::uintptr_t   phys_addr;
    PageTableObject* parent_pt;   // バインド中またはマップ先の親PT
    Capability*      map_next;    // 双方向リスト next
    Capability*      map_prev;    // 双方向リスト prev
};

struct PageTableData {
    PageTableObject* self_pt;         // 自身の PageTableObject
    PageTableObject* parent_pt;       // 親 PageTableObject
    Capability*      parent_map_next; // 親の mapped_slots_head_ リスト next
    Capability*      self_map_next;   // 自身の self_mapped_slots_head_ リスト next
};

static_assert(sizeof(FrameData) == 32);
static_assert(sizeof(PageTableData) == 32);

template <typename T>
concept ValidCapData = (sizeof(T) <= 32) && (alignof(T) <= 8);

template <typename T> struct CapTraits;
template <> struct CapTraits<FrameData> { static constexpr CapType expected_type = CapType::Frame; };
template <> struct CapTraits<PageTableData> { static constexpr CapType expected_type = CapType::PageTable; };

template <ValidCapData DataType>
class TypedCap {
private:
    Capability* slot_{nullptr};

public:
    TypedCap() = default;
    explicit TypedCap(Capability* slot) : slot_(slot) {}

    static TypedCap from(Capability* slot) {
        if (!slot || slot->is_null()) return TypedCap(nullptr);
        if constexpr (requires { CapTraits<DataType>::expected_type; }) {
            if (slot->type() != CapTraits<DataType>::expected_type) {
                // VSpace (level 4) と PageTable (level 1..3) の互換判定
                if (!(slot->type() == CapType::VSpace && CapTraits<DataType>::expected_type == CapType::PageTable)) {
                    return TypedCap(nullptr);
                }
            }
        }
        return TypedCap(slot);
    }

    bool is_valid() const { return slot_ != nullptr; }
    explicit operator bool() const { return is_valid(); }

    DataType* operator->() { return reinterpret_cast<DataType*>(slot_->data); }
    const DataType* operator->() const { return reinterpret_cast<const DataType*>(slot_->data); }

    Capability* raw_slot() const { return slot_; }
    CapRights rights() const { return slot_->rights(); }
    bool has_rights(CapRights r) const { return slot_->has_rights(r); }
};

inline void cdt_insert_child(Capability* parent, Capability* child) {
    if (!parent || !child) return;
    child->cdt_parent       = parent;
    child->cdt_next_child   = nullptr;
    child->cdt_next_sibling = parent->cdt_next_child;
    parent->cdt_next_child  = child;
}

inline bool cdt_move_slot(Capability* dest, Capability* src) {
    if (!dest || !src || dest == src || !dest->is_null() || src->is_null()) {
        return false;
    }

    for (int i = 0; i < 4; ++i) dest->data[i] = src->data[i];
    dest->flags            = src->flags;
    dest->cdt_parent       = src->cdt_parent;
    dest->cdt_next_child   = src->cdt_next_child;
    dest->cdt_next_sibling = src->cdt_next_sibling;

    if (Capability* parent = dest->cdt_parent) {
        if (parent->cdt_next_child == src) {
            parent->cdt_next_child = dest;
        } else {
            Capability* prev = parent->cdt_next_child;
            while (prev && prev->cdt_next_sibling != src) {
                prev = prev->cdt_next_sibling;
            }
            if (prev) {
                prev->cdt_next_sibling = dest;
            }
        }
    }

    for (Capability* c = dest->cdt_next_child; c != nullptr; c = c->cdt_next_sibling) {
        c->cdt_parent = dest;
    }

    relocate_pagetable_links_on_move(dest, src);

    src->clear_without_release();
    return true;
}

void cdt_revoke_subtree(Capability* root);

inline void cdt_remove_and_reparent(Capability* target) {
    if (!target || target->is_null()) return;

    if (target->has_slot_flag(SLOT_FLAG_IN_REVOKE)) {
        target->set_slot_flag(SLOT_FLAG_PENDING_DELETE);
        return;
    }

    if (target->type() == CapType::UntypedMemory && target->cdt_next_child != nullptr) {
        cdt_revoke_subtree(target);
        if (target->is_null()) return;
    }

    Capability* parent = target->cdt_parent;
    Capability* first_child = target->cdt_next_child;
    Capability* last_child = nullptr;
    for (Capability* c = first_child; c != nullptr; c = c->cdt_next_sibling) {
        c->cdt_parent = parent;
        last_child = c;
    }

    if (parent) {
        if (parent->cdt_next_child == target) {
            if (last_child) {
                last_child->cdt_next_sibling = target->cdt_next_sibling;
                parent->cdt_next_child = first_child;
            } else {
                parent->cdt_next_child = target->cdt_next_sibling;
            }
        } else {
            Capability* prev = parent->cdt_next_child;
            while (prev && prev->cdt_next_sibling != target) {
                prev = prev->cdt_next_sibling;
            }
            if (prev) {
                if (last_child) {
                    last_child->cdt_next_sibling = target->cdt_next_sibling;
                    prev->cdt_next_sibling = first_child;
                } else {
                    prev->cdt_next_sibling = target->cdt_next_sibling;
                }
            }
        }
    }

    target->cdt_parent       = nullptr;
    target->cdt_next_child   = nullptr;
    target->cdt_next_sibling = nullptr;
    target->release_and_clear();

    if (parent && parent->type() == CapType::UntypedMemory && parent->cdt_next_child == nullptr) {
        parent->set_untyped_watermark(0);
    }
}

inline void cdt_revoke_subtree(Capability* root) {
    if (!root || root->is_null()) return;

    root->set_slot_flag(SLOT_FLAG_IN_REVOKE);

    while (Capability* curr = root->cdt_next_child) {
        while (curr->cdt_next_child != nullptr) {
            curr = curr->cdt_next_child;
        }
        Capability* p = curr->cdt_parent;
        p->cdt_next_child = curr->cdt_next_sibling;

        curr->cdt_parent       = nullptr;
        curr->cdt_next_sibling = nullptr;
        if (curr->has_slot_flag(SLOT_FLAG_IN_REVOKE)) {
            curr->set_slot_flag(SLOT_FLAG_PENDING_DELETE);
        } else {
            curr->release_and_clear();
        }
    }

    if (root->type() == CapType::UntypedMemory) {
        root->set_untyped_watermark(0);
    }

    root->clear_slot_flag(SLOT_FLAG_IN_REVOKE);

    if (root->has_slot_flag(SLOT_FLAG_PENDING_DELETE)) {
        root->clear_slot_flag(SLOT_FLAG_PENDING_DELETE);
        cdt_remove_and_reparent(root);
    }
}

// Helper functions to initialize capability slots
inline void init_kernel_object_slot(Capability* slot, KernelObject* obj, CapRights rights, std::uint64_t badge = 0) {
    slot->clear_without_release();
    slot->data[0] = reinterpret_cast<std::uint64_t>(obj);
    slot->data[1] = badge;
    slot->flags = static_cast<std::uint8_t>(to_cap_type(obj->objectType()))
                | (static_cast<std::uint64_t>(SLOT_FLAG_HAS_KERNEL_OBJECT) << 8)
                | (static_cast<std::uint64_t>(rights) << 32);
}

inline void init_untyped_slot(Capability* slot, std::uintptr_t phys_base, std::size_t size_bytes, bool is_device = false) {
    slot->clear_without_release();
    slot->data[0] = phys_base;
    slot->data[1] = size_bytes;
    slot->data[2] = 0; // watermark
    slot->data[3] = is_device ? 1 : 0;
    CapRights r = CapRights::TRANSFER | CapRights::DERIVE | CapRights::REVOKE | CapRights::WRITE | CapRights::READ;
    slot->flags = static_cast<std::uint8_t>(CapType::UntypedMemory)
                | (static_cast<std::uint64_t>(r) << 32);
}

inline void init_frame_slot(Capability* slot, std::uintptr_t phys_addr, CapRights rights = CapRights::ALL, bool is_original = true) {
    slot->clear_without_release();
    slot->data[0] = phys_addr;
    slot->data[1] = 0;
    slot->data[2] = 0;
    slot->data[3] = 0;
    CapRights default_r = (rights == CapRights::ALL)
        ? (CapRights::DUPLICATE | CapRights::TRANSFER | CapRights::REVOKE | CapRights::READ | CapRights::WRITE | CapRights::EXEC | CapRights::NOCACHE)
        : rights;
    slot->flags = static_cast<std::uint8_t>(CapType::Frame)
                | (static_cast<std::uint64_t>(default_r) << 32);
    if (is_original) {
        slot->set_slot_flag(SLOT_FLAG_ORIGINAL);
    }
}

inline void init_frame_slot(Capability* slot, std::uintptr_t phys_addr, std::size_t /*size_bytes*/, CapRights rights = CapRights::ALL) {
    init_frame_slot(slot, phys_addr, rights, true);
}

inline void init_frame_slot(Capability* slot, std::uintptr_t phys_addr, std::size_t /*size_bytes*/, CapRights rights, bool is_original) {
    init_frame_slot(slot, phys_addr, rights, is_original);
}

inline void init_pagetable_object_slot(Capability* slot, PageTableObject* pt_obj, std::uint8_t level, CapRights rights = CapRights::ALL) {
    slot->clear_without_release();
    slot->data[0] = reinterpret_cast<std::uint64_t>(pt_obj);
    slot->data[1] = 0;
    slot->data[2] = 0;
    slot->data[3] = 0;
    CapRights default_r = (rights == CapRights::ALL)
        ? (CapRights::DUPLICATE | CapRights::TRANSFER | CapRights::REVOKE | CapRights::READ | CapRights::WRITE | CapRights::EXEC)
        : rights;
    CapType t = (level == 4) ? CapType::VSpace : CapType::PageTable;
    slot->flags = static_cast<std::uint8_t>(t)
                | (static_cast<std::uint64_t>(SLOT_FLAG_HAS_KERNEL_OBJECT) << 8)
                | (static_cast<std::uint64_t>(default_r) << 32);
}

inline void init_ioport_slot(Capability* slot, std::uint16_t base_port, std::uint32_t port_count) {
    slot->clear_without_release();
    slot->data[0] = static_cast<std::uint64_t>(base_port) | (static_cast<std::uint64_t>(port_count) << 32);
    CapRights r = CapRights::TRANSFER | CapRights::DERIVE | CapRights::REVOKE | CapRights::IN | CapRights::OUT;
    slot->flags = static_cast<std::uint8_t>(CapType::IOPort)
                | (static_cast<std::uint64_t>(r) << 32);
}

inline void init_irq_control_slot(Capability* slot) {
    slot->clear_without_release();
    CapRights r = CapRights::DUPLICATE | CapRights::TRANSFER | CapRights::REVOKE | CapRights::WRITE | CapRights::READ;
    slot->flags = static_cast<std::uint8_t>(CapType::IRQBatonControl)
                | (static_cast<std::uint64_t>(r) << 32);
}

inline void init_irq_handler_slot(Capability* slot, std::uint8_t vector, void* entry) {
    slot->clear_without_release();
    slot->data[0] = vector;
    slot->data[1] = reinterpret_cast<std::uint64_t>(entry);
    CapRights r = CapRights::TRANSFER | CapRights::REVOKE | CapRights::BIND | CapRights::ACK_WAIT;
    slot->flags = static_cast<std::uint8_t>(CapType::IRQHandler)
                | (static_cast<std::uint64_t>(r) << 32);
}

using CapID = std::uint32_t;
constexpr CapID INVALID_CAP_ID = 0xFFFFFFFF;

// Reserved slot range (slots 200..249: 50 slots):
// Reserved for system use: slots 200..239 are reserved for system UntypedMemory pools and internal buffers,
// and slots 240..249 are well-known capabilities (CAP_UNTYPED, CAP_BOOTINFO, CAP_SELF_CSPACE, etc.).
// Dynamic user allocations avoid this range to prevent collision with system initialization.
constexpr CapID CAP_RESERVED_SYSTEM_START = 200;
constexpr CapID CAP_RESERVED_SYSTEM_END   = 249;

inline bool is_reserved_system_slot(CapID slot) {
    return (slot >= CAP_RESERVED_SYSTEM_START && slot <= CAP_RESERVED_SYSTEM_END);
}

class CSpace {
public:
    static constexpr std::size_t MAX_CAPS = 4096;

private:
    Capability slots[MAX_CAPS];

public:
    CSpace() = default;
    CSpace(const CSpace&) = delete;
    CSpace& operator=(const CSpace&) = delete;

    Capability* getSlot(CapID id) {
        if (id < MAX_CAPS) {
            return &slots[id];
        }
        return nullptr;
    }

    const Capability* getSlot(CapID id) const {
        if (id < MAX_CAPS) {
            return &slots[id];
        }
        return nullptr;
    }

    bool is_range_null(CapID start_slot, std::size_t count) const {
        if (start_slot >= MAX_CAPS || count == 0 || count > MAX_CAPS - start_slot) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (!slots[start_slot + i].is_null()) {
                return false;
            }
        }
        return true;
    }

    bool remove(CapID id) {
        if (Capability* slot = getSlot(id)) {
            if (!slot->is_null()) {
                cdt_remove_and_reparent(slot);
                return true;
            }
        }
        return false;
    }

    bool revoke(CapID id) {
        if (Capability* slot = getSlot(id)) {
            if (!slot->is_null() && slot->has_rights(CapRights::REVOKE)) {
                cdt_revoke_subtree(slot);
                return true;
            }
        }
        return false;
    }

    // 互換ヘルパー: スロット直接指定での初期化
    Capability* get(CapID id) {
        return getSlot(id);
    }
};

} // namespace oz
