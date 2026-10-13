#pragma once

#include <cstdint>
#include <cstddef>


namespace oz {

enum class CapRights : std::uint32_t {
    NONE    = 0,
    READ    = 1 << 0,
    WRITE   = 1 << 1,
    EXEC    = 1 << 2,
    GRANT   = 1 << 3,
    ALL     = 0xFFFFFFFF
};

inline CapRights operator|(CapRights a, CapRights b) {
    return static_cast<CapRights>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline CapRights operator&(CapRights a, CapRights b) {
    return static_cast<CapRights>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

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
};

class KernelObject {
protected:
    // Starts at 1: the creator owns the initial reference and must call
    // release() once it has handed the object to a Capability/CSpace.
    std::uint32_t ref_count{1};
    ObjectType obj_type_{ObjectType::Unknown};
public:
    explicit KernelObject(ObjectType t = ObjectType::Unknown) : obj_type_(t) {}
    virtual ~KernelObject() = default;

    ObjectType objectType() const { return obj_type_; }

    void add_ref() { ref_count++; }
    void release() {
        if (ref_count > 0) {
            ref_count--;
            if (ref_count == 0) {
                delete this;
            }
        }
    }
    std::uint32_t get_ref_count() const { return ref_count; }
};

struct Capability {
    KernelObject* object;
    CapRights rights;

    Capability() : object(nullptr), rights(CapRights::NONE) {}
    Capability(KernelObject* obj, CapRights r) : object(obj), rights(r) {
        if (object) {
            object->add_ref();
        }
    }

    // Copy constructor
    Capability(const Capability& other) : object(other.object), rights(other.rights) {
        if (object) {
            object->add_ref();
        }
    }

    // Move constructor
    Capability(Capability&& other) noexcept : object(other.object), rights(other.rights) {
        other.object = nullptr;
        other.rights = CapRights::NONE;
    }

    // Copy assignment
    Capability& operator=(const Capability& other) {
        if (this != &other) {
            if (object) {
                object->release();
            }
            object = other.object;
            rights = other.rights;
            if (object) {
                object->add_ref();
            }
        }
        return *this;
    }

    // Move assignment
    Capability& operator=(Capability&& other) noexcept {
        if (this != &other) {
            if (object) {
                object->release();
            }
            object = other.object;
            rights = other.rights;
            other.object = nullptr;
            other.rights = CapRights::NONE;
        }
        return *this;
    }

    ~Capability() {
        if (object) {
            object->release();
        }
    }

    bool is_valid() const { return object != nullptr; }
};

using CapID = std::uint32_t;
constexpr CapID INVALID_CAP_ID = 0xFFFFFFFF;

class CSpace {
private:
    static constexpr std::size_t MAX_CAPS = 256;
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
