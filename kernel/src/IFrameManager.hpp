#pragma once
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include "Address.hpp"
#include "FlagClass.hpp"

namespace oz {

// 1次所有者の種別
enum class PageOwnerType : std::uint8_t {
    RESERVED = 0,
    FRAME_MANAGER = 1,
    PAGE_TABLE = 2,
    KMALLOCATOR = 3,
    USER_ANON = 4,
    OTHER = 5,
};

class DescriptorFlags : public utils::FlagClass<std::uint8_t, DescriptorFlags> {
    constexpr explicit DescriptorFlags(std::uint8_t flags) : FlagClass(flags) {}
public:
    static const DescriptorFlags NONE;
    static const DescriptorFlags FREE;
    static const DescriptorFlags HEAD;
    static const DescriptorFlags RESERVED;
};

inline constexpr DescriptorFlags DescriptorFlags::NONE     = DescriptorFlags(0);
inline constexpr DescriptorFlags DescriptorFlags::FREE     = DescriptorFlags(1 << 0);
inline constexpr DescriptorFlags DescriptorFlags::HEAD     = DescriptorFlags(1 << 1);
inline constexpr DescriptorFlags DescriptorFlags::RESERVED = DescriptorFlags(1 << 2);

struct alignas(32) PageFrameDescriptor {
    std::uint8_t ownerData[24];
    std::uint8_t level;
    PageOwnerType ownerType;
    DescriptorFlags flags;
    std::uint8_t reserved[5];
};

static_assert(sizeof(PageFrameDescriptor) == 32);
static_assert(alignof(PageFrameDescriptor) == 32);

struct BuddyFreeListEntry {
    static constexpr PageOwnerType OWNER_TYPE = PageOwnerType::FRAME_MANAGER;
    PageFrameDescriptor* next;
    PageFrameDescriptor* prev;
};

static_assert(sizeof(BuddyFreeListEntry) <= sizeof(PageFrameDescriptor::ownerData));

template <PageOwnerType OwnerType>
class PageOwnerBase {
public:
    static constexpr PageOwnerType OWNER_TYPE = OwnerType;
};

template <typename T>
concept page_owner = requires {
    { T::OWNER_TYPE } -> std::same_as<PageOwnerType>;
};

template <typename Metadata>
concept page_block_metadata = requires {
    sizeof(Metadata) <= sizeof(PageFrameDescriptor::ownerData);
    alignof(PageFrameDescriptor) % alignof(Metadata) == 0;
    { Metadata::OWNER_TYPE } -> std::same_as<PageOwnerType>;
};

template <typename MetadataType = std::monostate>
class PageBlock {
private:
    PageFrameDescriptor* pfd;

    template <typename>
    friend class PageBlock;

public:
    constexpr PageBlock() : pfd(nullptr) {}
    constexpr PageBlock(std::nullptr_t) : pfd(nullptr) {}
    explicit constexpr PageBlock(PageFrameDescriptor* pfd) : pfd(pfd) {}

    template <typename OtherMeta>
    constexpr PageBlock(const PageBlock<OtherMeta>& other) : pfd(other.pfd) {}

    const PageFrameDescriptor* operator->() const {
        return pfd;
    }
    PageFrameDescriptor* operator->() {
        return pfd;
    }

    MetadataType& operator[](std::size_t index) {
        return *reinterpret_cast<MetadataType*>(&(pfd + index)->ownerData);
    }
    const MetadataType& operator[](std::size_t index) const {
        return *reinterpret_cast<const MetadataType*>(&(pfd + index)->ownerData);
    }

    constexpr explicit operator bool() const {
        return pfd != nullptr;
    }
    constexpr bool operator==(const PageBlock& other) const {
        return pfd == other.pfd;
    }
    constexpr bool operator!=(const PageBlock& other) const {
        return pfd != other.pfd;
    }

    PageFrameDescriptor* getDescriptor() const {
        return pfd;
    }
    std::uint8_t getLevel() const {
        return pfd ? pfd->level : 0;
    }
    std::size_t size() const {
        return pfd ? (static_cast<std::size_t>(1) << pfd->level) : 0;
    }
    std::size_t pageCount() const {
        return size();
    }

    template <typename T>
        requires page_block_metadata<T>
    PageBlock<T> withMetadata() const {
        if (pfd == nullptr || pfd->ownerType != T::OWNER_TYPE) {
            return PageBlock<T>{nullptr};
        }
        return PageBlock<T>{pfd};
    }

    void setOwner(PageOwnerType ownerType) {
        if (pfd) pfd->ownerType = ownerType;
    }

    void constructAt(std::size_t index) {
        new (&(*this)[index]) MetadataType();
    }
    void constructAll() {
        for (std::size_t i = 0; i < size(); i++) {
            constructAt(i);
        }
    }
    void destructAt(std::size_t index) {
        (*this)[index].~MetadataType();
    }
    void destructAll() {
        for (std::size_t i = 0; i < size(); i++) {
            destructAt(i);
        }
    }
};

template <typename FrameManager>
concept frame_manager = requires(FrameManager fm, std::uint8_t level, PageBlock<> block, PageFrameDescriptor* pfd, PhysicalAddress<void> phys) {
    { fm.FRAME_SIZE } -> std::convertible_to<std::size_t>;
    { fm.allocateBlock(level) } -> std::same_as<PageBlock<>>;
    { fm.freeBlock(block) } -> std::same_as<void>;
    { fm.freeBlock(pfd) } -> std::same_as<void>;
    { fm.getPhysicalAddress(block) } -> std::same_as<PhysicalAddress<void>>;
    { fm.getPhysicalAddress(pfd) } -> std::same_as<PhysicalAddress<void>>;
    { fm.getDescriptor(phys) } -> std::same_as<PageFrameDescriptor*>;
};

template <typename Accessor>
concept frame_manager_accessor = requires() {
    typename Accessor::Settings::FrameManager;
    {Accessor::getFrameManager()} -> std::same_as<typename Accessor::Settings::FrameManager&>;
} && frame_manager<typename Accessor::Settings::FrameManager>;

}  // namespace oz