#pragma once
#include <array>
#include "memory/Address.hpp"
#include "memory/IFrameManager.hpp"
#include "utils/utils.hpp"

namespace oz {

template <frame_manager_accessor Accessor>
class TLSFMemoryAllocator {
private:
    struct BoundaryTag {
        union {
            std::uint64_t back_size_and_flags;
            PageFrameDescriptor* frameDescriptorPtr; // isLargeが立っているとき有効
        };
        std::uint64_t size_and_flags;
        union {
            BoundaryTag* prev_link;
            std::uint8_t data;
        };
        BoundaryTag* next_link;
        static constexpr std::uint64_t thisIsUsed = 0b0001;
        static constexpr std::uint64_t backIsUsed = 0b0010;
        static constexpr std::uint64_t isLarge = 0b0100;
        static constexpr std::uint64_t allMask = 0b1111;

        void removeFromList() {
            this->prev_link->next_link = this->next_link;
            if (this->next_link != nullptr) {
                this->next_link->prev_link = this->prev_link;
            }
            this->next_link = nullptr;
            this->prev_link = nullptr;
        }

        void setFlags(std::uint64_t flagBits) {
            this->size_and_flags |= (flagBits & 0b1111);
        }

        void setSize(std::uint64_t size) {
            this->size_and_flags = (size & ~0b1111) | (this->size_and_flags & 0b1111);
        }

        void clearFlags(std::uint64_t flagBits) {
            this->size_and_flags &= ~(flagBits & 0b1111);
        }

        BoundaryTag* getForward() {
            return reinterpret_cast<BoundaryTag*>(
                reinterpret_cast<std::uint8_t*>(this) + this->getSize());
        }

        BoundaryTag* getBack() {
            return reinterpret_cast<BoundaryTag*>(
                reinterpret_cast<std::uint8_t*>(this) -
                (this->back_size_and_flags & ~(0b1111)));
        }

        bool test(std::uint64_t testFlagBits) {
            return (testFlagBits == (this->size_and_flags & testFlagBits));
        }

        std::size_t getSize() {
            return (this->size_and_flags & ~(0b1111));
        }
    };
    static_assert(sizeof(BoundaryTag) == 32, "");

    struct FreeList {
        BoundaryTag* link;

        BoundaryTag* popFront() {
            BoundaryTag* ret = this->link;
            this->link = ret->next_link;
            if (this->link != nullptr) {
                this->link->prev_link = ret->prev_link;
            }
            ret->next_link = nullptr;
            ret->prev_link = nullptr;
            return ret;
        }

        void pushFront(BoundaryTag* target) {
            target->next_link = nullptr;
            if (this->link != nullptr) {
                target->next_link = this->link;
                target->prev_link = this->link->prev_link;
                target->next_link->prev_link = target;
            } else {
                target->prev_link = reinterpret_cast<BoundaryTag*>(
                    reinterpret_cast<std::uint8_t*>(this) -
                    (sizeof(BoundaryTag::prev_link) +
                     sizeof(BoundaryTag::size_and_flags) +
                     sizeof(BoundaryTag::back_size_and_flags)));
            }
            this->link = target;
        }
    };

    using TLI = std::array<std::size_t, 2>;
    static constexpr std::size_t size_align_2order = 4;
    FreeList* tlsf_table;
    std::uint32_t bitMap_not_empty_freelist_second_level[64] = {};
    std::uint64_t bitMap_not_empty_freelist_first_level = 0;
    std::size_t max_log2_SLI;
    std::size_t framePerChunk;
    std::size_t max_size_of_block;
    std::size_t min_size_of_block;

    std::size_t convertTLItoLinearIndex(TLI& tli) {
        return (tli[0] << max_log2_SLI) + tli[1];
    }

    TLI convertLinearIndexToTLI(std::size_t linear) {
        std::uint64_t sli_mask = 0;
        for (std::size_t i = 0; i < max_log2_SLI; i++) {
            sli_mask |= (0b1 << i);
        }
        TLI tli = {};
        tli[1] = linear & sli_mask;
        tli[0] = linear >> max_log2_SLI;
        return tli;
    }

    TLI convertSizeToTLI(std::size_t size) {
        std::uint64_t sli_mask = 0;
        for (std::size_t i = 0; i < max_log2_SLI; i++) {
            sli_mask |= (0b1 << i);
        }
        std::size_t msb = oz::utils::getMSB(size);
        TLI tli = {};
        if (msb < (max_log2_SLI + size_align_2order)) {
            tli[0] = 0;
            tli[1] = (size >> size_align_2order) & sli_mask;
        } else {
            tli[0] = msb - (max_log2_SLI + size_align_2order) + 1;
            tli[1] = (size >> (size_align_2order + tli[0] - 1)) & sli_mask;
        }
        return tli;
    }

    std::size_t convertSizeToIndex(std::size_t size) {
        TLI tli = convertSizeToTLI(size);
        return convertTLItoLinearIndex(tli);
    }

    BoundaryTag* newBlock() {
        constexpr auto& frameManager = Accessor::getFrameManager();
        std::uint8_t level = 0;
        while ((1ULL << level) < framePerChunk) {
            level++;
        }
        PageBlock block = frameManager.allocateBlock(level);
        if (!block) return nullptr;
        block.setOwner(PageOwnerType::KMALLOCATOR);

        BoundaryTag* ret =
            reinterpret_cast<BoundaryTag*>(oz::phys_to_virt(frameManager.getPhysicalAddress(block)));
        std::size_t size = (frameManager.FRAME_SIZE << level) -
                           (sizeof(BoundaryTag::back_size_and_flags) +
                            sizeof(BoundaryTag::size_and_flags));
        ret->setSize(size);
        ret->clearFlags(BoundaryTag::allMask);
        ret->setFlags(BoundaryTag::backIsUsed);
        ret->getForward()->setFlags(BoundaryTag::thisIsUsed);
        return ret;
    }

    BoundaryTag* mallocLarge(std::size_t size) {
        constexpr auto& frameManager = Accessor::getFrameManager();
        std::size_t totalBytes = size + sizeof(BoundaryTag::back_size_and_flags) + sizeof(BoundaryTag::size_and_flags);
        std::size_t numFrames = (totalBytes + frameManager.FRAME_SIZE - 1) / frameManager.FRAME_SIZE;
        std::uint8_t level = 0;
        while ((1ULL << level) < numFrames) {
            level++;
        }
        PageBlock block = frameManager.allocateBlock(level);
        if (!block) return nullptr;
        block.setOwner(PageOwnerType::KMALLOCATOR);

        BoundaryTag* ret =
            reinterpret_cast<BoundaryTag*>(oz::phys_to_virt(frameManager.getPhysicalAddress(block)));
        ret->frameDescriptorPtr = block.getDescriptor();
        ret->size_and_flags = (static_cast<std::uint64_t>(level) << 4) | BoundaryTag::isLarge;
        return ret;
    }

    void freeLarge(BoundaryTag* returnedBlock) {
        constexpr auto& frameManager = Accessor::getFrameManager();
        PageFrameDescriptor* pfd = returnedBlock->frameDescriptorPtr;
        frameManager.freeBlock(pfd);
    }

    void checkAndClearBitMap(std::size_t linearIndex) {
        TLI tli = convertLinearIndexToTLI(linearIndex);
        if (tlsf_table[linearIndex].link == nullptr) {
            bitMap_not_empty_freelist_second_level[tli[0]] &= ~(0b1 << tli[1]);
            if (bitMap_not_empty_freelist_second_level[tli[0]] == 0) {
                bitMap_not_empty_freelist_first_level &= ~(0b1 << tli[0]);
            }
        }
    }

    void checkAndClearBitMap(TLI& tli) {
        std::size_t linearIndex = convertTLItoLinearIndex(tli);
        if (tlsf_table[linearIndex].link == nullptr) {
            bitMap_not_empty_freelist_second_level[tli[0]] &= ~(0b1 << tli[1]);
            if (bitMap_not_empty_freelist_second_level[tli[0]] == 0) {
                bitMap_not_empty_freelist_first_level &= ~(0b1 << tli[0]);
            }
        }
    }

    void setBitMap(std::size_t linearIndex) {
        TLI tli = convertLinearIndexToTLI(linearIndex);
        setBitMap(tli);
    }

    void setBitMap(TLI& tli) {
        bitMap_not_empty_freelist_second_level[tli[0]] |= 0b1 << tli[1];
        bitMap_not_empty_freelist_first_level |= 0b1 << tli[0];
    }

public:
    TLSFMemoryAllocator(std::size_t _framePerChunk = 1, std::size_t _max_Log2_SLI = 5)
        : max_log2_SLI{_max_Log2_SLI},
          framePerChunk{_framePerChunk} {
        constexpr auto& fm = Accessor::getFrameManager();
        TLI max_tli = convertSizeToTLI(fm.FRAME_SIZE * framePerChunk);
        std::size_t needTableSize = convertTLItoLinearIndex(max_tli) + 1;

        std::size_t needTableBytes = needTableSize * sizeof(FreeList);
        std::size_t needFrameForTable =
            (needTableBytes + fm.FRAME_SIZE - 1) / fm.FRAME_SIZE;
        std::uint8_t level = 0;
        while ((1ULL << level) < needFrameForTable) {
            level++;
        }
        PageBlock block = fm.allocateBlock(level);
        block.setOwner(PageOwnerType::KMALLOCATOR);
        tlsf_table = reinterpret_cast<FreeList*>(oz::phys_to_virt(fm.getPhysicalAddress(block)));

        for (std::size_t i = 0; i < needTableSize; i++) {
            tlsf_table[i].link = nullptr;
        }

        max_size_of_block = fm.FRAME_SIZE * framePerChunk -
                            (sizeof(BoundaryTag::back_size_and_flags) +
                             sizeof(BoundaryTag::size_and_flags));
        min_size_of_block = sizeof(BoundaryTag);
    }

    void* malloc(std::size_t size) {
        size = (size + sizeof(BoundaryTag::size_and_flags) + 15) &
               ~(0b1111);  // 16Byte align
        if (size < min_size_of_block) {
            size = min_size_of_block;
        }
        if (size > max_size_of_block) {
            return &mallocLarge(size)->data;
        }

        TLI tli = convertSizeToTLI(size);
        std::size_t index = convertTLItoLinearIndex(tli);

        if (tlsf_table[index].link != nullptr) {
            BoundaryTag* allocatingBlock = tlsf_table[index].popFront();
            checkAndClearBitMap(tli);
            allocatingBlock->setFlags(BoundaryTag::thisIsUsed);
            allocatingBlock->getForward()->setFlags(BoundaryTag::backIsUsed);
            return &allocatingBlock->data;
        } else {
            std::uint64_t mask_sli = ~(0ULL) << (tli[1] + 1);
            std::uint64_t bitMap_isExistsSecond =
                bitMap_not_empty_freelist_second_level[tli[0]] & mask_sli;

            BoundaryTag* allocatingBlock = nullptr;

            if (bitMap_isExistsSecond == 0) {
                std::uint64_t mask_fli = ~(0ULL) << (tli[0] + 1);
                std::uint64_t bitMap_isExistsFirst =
                    bitMap_not_empty_freelist_first_level & mask_fli;

                if (bitMap_isExistsFirst == 0) {
                    allocatingBlock = newBlock();
                    if (!allocatingBlock) return nullptr;
                } else {
                    tli[0] = oz::utils::getLSB(bitMap_isExistsFirst);
                    tli[1] = oz::utils::getLSB(
                        bitMap_not_empty_freelist_second_level[tli[0]]);
                    allocatingBlock =
                        tlsf_table[convertTLItoLinearIndex(tli)].popFront();
                    checkAndClearBitMap(tli);
                }
            } else {
                tli[1] = oz::utils::getLSB(bitMap_isExistsSecond);
                allocatingBlock =
                    tlsf_table[convertTLItoLinearIndex(tli)].popFront();
                checkAndClearBitMap(tli);
            }

            if (allocatingBlock->getSize() >= size + min_size_of_block) {
                BoundaryTag* forkedBlock = reinterpret_cast<BoundaryTag*>(
                    reinterpret_cast<std::uint8_t*>(allocatingBlock) + size);

                std::uint64_t forkedBlockSize = allocatingBlock->getSize() - size;

                allocatingBlock->setSize(size);
                forkedBlock->setSize(forkedBlockSize);
                forkedBlock->clearFlags(BoundaryTag::allMask);
                forkedBlock->getForward()->clearFlags(BoundaryTag::backIsUsed);
                forkedBlock->getForward()->back_size_and_flags =
                    forkedBlock->size_and_flags;
                std::size_t linear = convertSizeToIndex(forkedBlockSize) - 1;
                tlsf_table[linear].pushFront(forkedBlock);
                setBitMap(linear);
            }

            allocatingBlock->setFlags(BoundaryTag::thisIsUsed);
            allocatingBlock->getForward()->setFlags(BoundaryTag::backIsUsed);
            return &allocatingBlock->data;
        }
    }

    void free(void* ptr) {
        if (ptr == nullptr) return;

        BoundaryTag* retBlock = reinterpret_cast<BoundaryTag*>(
            reinterpret_cast<std::uint8_t*>(ptr) -
            (sizeof(BoundaryTag::size_and_flags) +
             sizeof(BoundaryTag::back_size_and_flags)));

        if (retBlock->test(BoundaryTag::isLarge)) {
            freeLarge(retBlock);
            return;
        }

        if (!retBlock->test(BoundaryTag::backIsUsed)) {
            BoundaryTag* back = retBlock->getBack();
            back->removeFromList();
            checkAndClearBitMap(convertSizeToIndex(back->getSize()) - 1);
            back->size_and_flags += retBlock->size_and_flags & ~(0b1111);
            retBlock = back;
        }

        BoundaryTag* forward = retBlock->getForward();
        if (!forward->test(BoundaryTag::thisIsUsed)) {
            forward->removeFromList();
            checkAndClearBitMap(convertSizeToIndex(forward->getSize()) - 1);
            retBlock->size_and_flags += forward->size_and_flags & ~(0b1111);
        }

        retBlock->clearFlags(BoundaryTag::thisIsUsed);
        retBlock->getForward()->back_size_and_flags = retBlock->size_and_flags;
        retBlock->getForward()->clearFlags(BoundaryTag::backIsUsed);
        std::size_t index = convertSizeToIndex(retBlock->getSize()) - 1;
        tlsf_table[index].pushFront(retBlock);
        setBitMap(index);
    }
};

} // namespace oz