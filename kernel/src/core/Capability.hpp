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

inline CapRights operator|(CapRights a, CapRights b) {
    return static_cast<CapRights>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline CapRights operator&(CapRights a, CapRights b) {
    return static_cast<CapRights>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

inline CapRights operator~(CapRights a) {
    return static_cast<CapRights>(~static_cast<std::uint32_t>(a));
}

enum class CapKind : std::uint8_t {
    Duplicable = 0,
    Linear     = 1,
};

enum class ObjectType : std::uint8_t {
    Unknown = 0,
    ChannelEndpoint,
    Frame,
    UntypedMemory,
    VSpace,
    TCB,
    CSpaceCap,
    IRQHandler,
    IOPort,
    PageTable,
    Notification,
    Baton,
    InterruptThread,
};

struct Capability;

class KernelObject {
protected:
    // Starts at 1: the creator owns the initial reference and must call
    // release() once it has handed the object to a Capability/CSpace.
    std::uint32_t ref_count{1};
    ObjectType obj_type_{ObjectType::Unknown};
    bool is_untyped_backed_{false};
    bool is_revoked_{false};
    CapKind cap_kind_{CapKind::Duplicable};
    std::uint32_t active_cap_count_{0};

    KernelObject* cdt_parent_{nullptr};
    KernelObject* cdt_first_child_{nullptr};
    KernelObject* cdt_next_sibling_{nullptr};
    KernelObject* cdt_prev_sibling_{nullptr};

    Capability* cap_list_head_{nullptr};

public:
    explicit KernelObject(ObjectType t = ObjectType::Unknown, bool untyped = false, CapKind kind = CapKind::Duplicable)
        : obj_type_(t), is_untyped_backed_(untyped), cap_kind_(kind) {
        if (t == ObjectType::IRQHandler || t == ObjectType::IOPort) {
            cap_kind_ = CapKind::Linear;
        }
    }
    virtual ~KernelObject();

    ObjectType objectType() const { return obj_type_; }
    bool isUntypedBacked() const { return is_untyped_backed_; }
    void setUntypedBacked(bool untyped) { is_untyped_backed_ = untyped; }

    bool isRevoked() const { return is_revoked_; }
    void markRevoked() { is_revoked_ = true; }

    CapKind capKind() const { return cap_kind_; }
    void setCapKind(CapKind k) { cap_kind_ = k; }
    bool isLinear() const { return cap_kind_ == CapKind::Linear; }
    std::uint32_t activeCapCount() const { return active_cap_count_; }

    KernelObject* cdtParent() const { return cdt_parent_; }
    KernelObject* cdtFirstChild() const { return cdt_first_child_; }
    KernelObject* cdtNextSibling() const { return cdt_next_sibling_; }

    void addChild(KernelObject* child) {
        if (!child || child == this) return;
        child->cdt_parent_ = this;
        child->cdt_next_sibling_ = cdt_first_child_;
        child->cdt_prev_sibling_ = nullptr;
        if (cdt_first_child_) {
            cdt_first_child_->cdt_prev_sibling_ = child;
        }
        cdt_first_child_ = child;
    }

    void removeChild(KernelObject* child) {
        if (!child) return;
        if (child->cdt_prev_sibling_) {
            child->cdt_prev_sibling_->cdt_next_sibling_ = child->cdt_next_sibling_;
        } else if (cdt_first_child_ == child) {
            cdt_first_child_ = child->cdt_next_sibling_;
        }
        if (child->cdt_next_sibling_) {
            child->cdt_next_sibling_->cdt_prev_sibling_ = child->cdt_prev_sibling_;
        }
        child->cdt_parent_ = nullptr;
        child->cdt_next_sibling_ = nullptr;
        child->cdt_prev_sibling_ = nullptr;
    }

    void detachFromParent() {
        if (cdt_parent_) {
            cdt_parent_->removeChild(this);
        }
    }

    void attachCapability(Capability* cap);
    void detachCapability(Capability* cap);
    void invalidateAllCapabilities();

    void add_ref() { ref_count++; }

    virtual void destroy() {
        if (!is_untyped_backed_) {
            delete this;
        } else {
            this->~KernelObject();
        }
    }

    void release() {
        if (ref_count > 0) {
            ref_count--;
            if (ref_count == 0) {
                destroy();
            }
        }
    }
    std::uint32_t get_ref_count() const { return ref_count; }
};

struct Capability {
    KernelObject* object{nullptr};
    CapRights rights{CapRights::NONE};
    Capability* next_cap_{nullptr};
    Capability* prev_cap_{nullptr};

    Capability() : object(nullptr), rights(CapRights::NONE), next_cap_(nullptr), prev_cap_(nullptr) {}

    Capability(KernelObject* obj, CapRights r)
        : object(obj), rights(r), next_cap_(nullptr), prev_cap_(nullptr) {
        if (object) {
            object->add_ref();
            object->attachCapability(this);
        }
    }

    // Copy constructor
    Capability(const Capability& other)
        : object(other.object), rights(other.rights), next_cap_(nullptr), prev_cap_(nullptr) {
        if (object) {
            object->add_ref();
            object->attachCapability(this);
        }
    }

    // Move constructor
    Capability(Capability&& other) noexcept
        : object(other.object), rights(other.rights), next_cap_(nullptr), prev_cap_(nullptr) {
        if (object) {
            other.object->detachCapability(&other);
            object->attachCapability(this);
        }
        other.object = nullptr;
        other.rights = CapRights::NONE;
        other.next_cap_ = nullptr;
        other.prev_cap_ = nullptr;
    }

    // Copy assignment
    Capability& operator=(const Capability& other) {
        if (this != &other) {
            if (object) {
                object->detachCapability(this);
                object->release();
            }
            object = other.object;
            rights = other.rights;
            next_cap_ = nullptr;
            prev_cap_ = nullptr;
            if (object) {
                object->add_ref();
                object->attachCapability(this);
            }
        }
        return *this;
    }

    // Move assignment
    Capability& operator=(Capability&& other) noexcept {
        if (this != &other) {
            if (object) {
                object->detachCapability(this);
                object->release();
            }
            object = other.object;
            rights = other.rights;
            next_cap_ = nullptr;
            prev_cap_ = nullptr;
            if (object) {
                other.object->detachCapability(&other);
                object->attachCapability(this);
            }
            other.object = nullptr;
            other.rights = CapRights::NONE;
            other.next_cap_ = nullptr;
            other.prev_cap_ = nullptr;
        }
        return *this;
    }

    ~Capability() {
        if (object) {
            object->detachCapability(this);
            object->release();
            object = nullptr;
        }
    }

    bool is_valid() const { return object != nullptr && !object->isRevoked(); }
    bool is_linear() const { return object != nullptr && object->isLinear(); }
    bool is_once() const { return (rights & CapRights::ONCE) != CapRights::NONE; }
    bool can_duplicate() const {
        return is_valid() && !is_linear() && ((rights & CapRights::DUPLICATE) == CapRights::DUPLICATE);
    }
};

inline KernelObject::~KernelObject() {
    invalidateAllCapabilities();
    detachFromParent();
}

inline void KernelObject::attachCapability(Capability* cap) {
    if (!cap) return;
    cap->next_cap_ = cap_list_head_;
    cap->prev_cap_ = nullptr;
    if (cap_list_head_) {
        cap_list_head_->prev_cap_ = cap;
    }
    cap_list_head_ = cap;
    active_cap_count_++;
}

inline void KernelObject::detachCapability(Capability* cap) {
    if (!cap) return;
    if (cap->prev_cap_) {
        cap->prev_cap_->next_cap_ = cap->next_cap_;
    } else if (cap_list_head_ == cap) {
        cap_list_head_ = cap->next_cap_;
    }
    if (cap->next_cap_) {
        cap->next_cap_->prev_cap_ = cap->prev_cap_;
    }
    cap->next_cap_ = nullptr;
    cap->prev_cap_ = nullptr;
    if (active_cap_count_ > 0) {
        active_cap_count_--;
    }
}

inline void KernelObject::invalidateAllCapabilities() {
    Capability* curr = cap_list_head_;
    cap_list_head_ = nullptr;
    active_cap_count_ = 0;
    std::size_t revoked_refs = 0;
    while (curr) {
        Capability* next = curr->next_cap_;
        curr->object = nullptr;
        curr->rights = CapRights::NONE;
        curr->next_cap_ = nullptr;
        curr->prev_cap_ = nullptr;
        revoked_refs++;
        curr = next;
    }
    for (std::size_t i = 0; i < revoked_refs; ++i) {
        this->release();
    }
}

using CapID = std::uint32_t;
constexpr CapID INVALID_CAP_ID = 0xFFFFFFFF;

class CSpace {
private:
    static constexpr std::size_t MAX_CAPS = 1024;
    Capability slots[MAX_CAPS];
public:
    CSpace() = default;
    
    CapID add(const Capability& cap) {
        for (std::size_t i = 0; i < MAX_CAPS; ++i) {
            if (!slots[i].is_valid()) {
                slots[i] = cap;
                return static_cast<CapID>(i);
            }
        }
        return INVALID_CAP_ID;
    }

    Capability get(CapID id) const {
        if (id < MAX_CAPS) {
            if (slots[id].object && slots[id].object->isRevoked()) {
                return Capability();
            }
            return slots[id];
        }
        return Capability();
    }

    bool insert(CapID id, const Capability& cap) {
        if (id < MAX_CAPS) {
            slots[id] = cap;
            return true;
        }
        return false;
    }

    bool remove(CapID id) {
        if (id < MAX_CAPS && slots[id].is_valid()) {
            slots[id] = Capability(); // Replace with invalid, triggering release
            return true;
        }
        return false;
    }
};

} // namespace oz
