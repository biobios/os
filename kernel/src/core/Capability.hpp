#pragma once

#include <oz/CapabilityTypes.hpp>
#include <cstdint>
#include <cstddef>

namespace oz {

class PageTableObject;
struct Capability;
void relocate_pagetable_links_on_move(Capability* dest, Capability* src);

constexpr std::uint8_t SLOT_FLAG_HAS_KERNEL_OBJECT = 1 << 0;
constexpr std::uint8_t SLOT_FLAG_IN_REVOKE         = 1 << 1;
constexpr std::uint8_t SLOT_FLAG_PENDING_DELETE    = 1 << 2;
constexpr std::uint8_t SLOT_FLAG_IS_MAPPED         = 1 << 3;
constexpr std::uint8_t SLOT_FLAG_PT_BACKING        = 1 << 4;
constexpr std::uint8_t SLOT_FLAG_ORIGINAL          = 1 << 5;

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

void cdt_insert_child(Capability* parent, Capability* child);
bool cdt_move_slot(Capability* dest, Capability* src);
void cdt_remove_and_reparent(Capability* target);
void cdt_revoke_subtree(Capability* root);

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
