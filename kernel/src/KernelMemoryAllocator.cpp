#include "KernelMemoryAllocator.hpp"

#include "utils.hpp"

std::size_t oz::TLSFMemoryAllocator::convertTLItoLinearIndex(TLI& tli) {
    return (tli[0] << max_log2_SLI) + tli[1];
}

oz::TLSFMemoryAllocator::TLI oz::TLSFMemoryAllocator::convertLinearIndexToTLI(
    std::size_t linear) {
    std::uint64_t sli_mask = 0;
    for (std::size_t i = 0; i < max_log2_SLI; i++) {
        sli_mask |= (0b1 << i);
    }

    TLI tli = {};
    tli[1] = linear & sli_mask;
    tli[0] = linear >> max_log2_SLI;
    return tli;
}

oz::TLSFMemoryAllocator::TLI oz::TLSFMemoryAllocator::convertSizeToTLI(
    std::size_t size) {
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

std::size_t oz::TLSFMemoryAllocator::convertSizeToIndex(std::size_t size) {
    TLI tli = convertSizeToTLI(size);
    return convertTLItoLinearIndex(tli);
}

oz::TLSFMemoryAllocator::BoundaryTag* oz::TLSFMemoryAllocator::newBlock() {
    FrameInfo* frameInfo = frameManager->allocatePages(framePerChunk);
    BoundaryTag* ret =
        reinterpret_cast<BoundaryTag*>(frameInfo->physicalAddress);
    std::size_t size = frameManager->FRAME_SIZE * framePerChunk -
                       (sizeof(BoundaryTag::back_size_and_flags) +
                        sizeof(BoundaryTag::size_and_flags));
    ret->setSize(size);
    // 存在しないブロックのマージを防止するために使用中フラグを立てる
    ret->clearFlags(BoundaryTag::allMask);
    ret->setFlags(BoundaryTag::backIsUsed);
    ret->getForward()->setFlags(BoundaryTag::thisIsUsed);
    return ret;
}

oz::TLSFMemoryAllocator::BoundaryTag* oz::TLSFMemoryAllocator::mallocLarge(
    std::size_t size) {
    std::size_t numFrames = (size + sizeof(BoundaryTag::back_size_and_flags) +
                             frameManager->FRAME_SIZE - 1) /
                                frameManager->FRAME_SIZE -
                            1;
    FrameInfo* frameInfo = frameManager->allocatePages(numFrames);
    BoundaryTag* ret =
        reinterpret_cast<BoundaryTag*>(frameInfo->physicalAddress);
    ret->frameInfoPtr = frameInfo;
    ret->size_and_flags = (numFrames << 4) | BoundaryTag::isLarge;
    return ret;
}

void oz::TLSFMemoryAllocator::freeLarge(BoundaryTag* returnedBlock) {
    std::size_t numFrames = returnedBlock->size_and_flags >> 4;
    frameManager->freePages(returnedBlock->frameInfoPtr, numFrames);
}

void oz::TLSFMemoryAllocator::checkAndClearBitMap(std::size_t linearIndex) {
    TLI tli = convertLinearIndexToTLI(linearIndex);
    if (tlsf_table[linearIndex].link == nullptr) {
        bitMap_not_empty_freelist_second_level[tli[0]] &= ~(0b1 << tli[1]);
        if (bitMap_not_empty_freelist_second_level[tli[0]] == 0) {
            bitMap_not_empty_freelist_first_level &= ~(0b1 << tli[0]);
        }
    }
}

void oz::TLSFMemoryAllocator::checkAndClearBitMap(TLI& tli) {
    std::size_t linearIndex = convertTLItoLinearIndex(tli);
    if (tlsf_table[linearIndex].link == nullptr) {
        bitMap_not_empty_freelist_second_level[tli[0]] &= ~(0b1 << tli[1]);
        if (bitMap_not_empty_freelist_second_level[tli[0]] == 0) {
            bitMap_not_empty_freelist_first_level &= ~(0b1 << tli[0]);
        }
    }
}

void oz::TLSFMemoryAllocator::setBitMap(std::size_t linearIndex) {
    TLI tli = convertLinearIndexToTLI(linearIndex);
    setBitMap(tli);
}

void oz::TLSFMemoryAllocator::setBitMap(TLI& tli) {
    bitMap_not_empty_freelist_second_level[tli[0]] |= 0b1 << tli[1];
    bitMap_not_empty_freelist_first_level |= 0b1 << tli[0];
}

oz::TLSFMemoryAllocator::TLSFMemoryAllocator(IFrameManager* fm,
                                             std::size_t _framePerChunk,
                                             std::size_t _max_Log2_SLI)
    : max_log2_SLI{_max_Log2_SLI},
      framePerChunk{_framePerChunk},
      frameManager{fm} {
    TLI max_tli = convertSizeToTLI(fm->FRAME_SIZE * framePerChunk);
    std::size_t needTableSize = convertTLItoLinearIndex(max_tli) + 1;

    std::size_t needFrameForTable =
        (needTableSize * sizeof(FreeList) + fm->FRAME_SIZE - 1) /
        fm->FRAME_SIZE;
    FrameInfo* frameInfo = fm->allocatePages(needFrameForTable);
    tlsf_table = reinterpret_cast<FreeList*>(frameInfo->physicalAddress);

    for (std::size_t i = 0; i < needTableSize; i++) {
        tlsf_table[i].link = nullptr;
    }

    max_size_of_block = fm->FRAME_SIZE * framePerChunk -
                        (sizeof(BoundaryTag::back_size_and_flags) +
                         sizeof(BoundaryTag::size_and_flags));
    min_size_of_block = sizeof(BoundaryTag);
}

void* oz::TLSFMemoryAllocator::malloc(std::size_t size) {
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
        // フリーブロックリストから一つ取り出す
        BoundaryTag* allocatingBlock = tlsf_table[index].popFront();
        checkAndClearBitMap(tli);
        // allocatingBlockを確保状態にする
        allocatingBlock->setFlags(BoundaryTag::thisIsUsed);
        allocatingBlock->getForward()->setFlags(BoundaryTag::backIsUsed);
        // ポインタを返す
        return &allocatingBlock->data;
    } else {
        std::uint64_t mask_sli = ~(0) << (tli[1] + 1);
        std::uint64_t bitMap_isExistsSecond =
            bitMap_not_empty_freelist_second_level[tli[0]] & mask_sli;

        BoundaryTag* allocatingBlock = nullptr;

        // なかったらFirstLevelから探す
        if (bitMap_isExistsSecond == 0) {
            std::uint64_t mask_fli = ~(0) << (tli[0] + 1);
            std::uint64_t bitMap_isExistsFirst =
                bitMap_not_empty_freelist_first_level & mask_fli;

            // 空き容量がなかったら新しくページを要求する
            if (bitMap_isExistsFirst == 0) {
                allocatingBlock = newBlock();
            } else {  // あったらそれ
                tli[0] = oz::utils::getLSB(bitMap_isExistsFirst);
                tli[1] = oz::utils::getLSB(
                    bitMap_not_empty_freelist_second_level[tli[0]]);
                allocatingBlock =
                    tlsf_table[convertTLItoLinearIndex(tli)].popFront();
                checkAndClearBitMap(tli);
            }
        } else {  // あったらそれ
            tli[1] = oz::utils::getLSB(bitMap_isExistsSecond);
            allocatingBlock =
                tlsf_table[convertTLItoLinearIndex(tli)].popFront();
            checkAndClearBitMap(tli);
        }

        if (allocatingBlock->getSize() >=
            size + min_size_of_block) {  // 分割可能
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

void oz::TLSFMemoryAllocator::free(void* ptr) {
    if (ptr == nullptr) return;

    // BoundaryTagに変換
    BoundaryTag* retBlock = reinterpret_cast<BoundaryTag*>(
        reinterpret_cast<std::uint8_t*>(ptr) -
        (sizeof(BoundaryTag::size_and_flags) +
         sizeof(BoundaryTag::back_size_and_flags)));
    // もしisLargeならfreeLarge
    if (retBlock->test(BoundaryTag::isLarge)) {
        freeLarge(retBlock);
        return;
    }
    // 前者が未使用ならマージ
    if (!retBlock->test(BoundaryTag::backIsUsed)) {
        BoundaryTag* back = retBlock->getBack();
        back->removeFromList();
        checkAndClearBitMap(convertSizeToIndex(back->getSize()) - 1);
        back->size_and_flags += retBlock->size_and_flags & ~(0b1111);
        retBlock = back;
    }
    // 後者が未使用ならマージ
    BoundaryTag* forward = retBlock->getForward();
    if (!forward->test(BoundaryTag::thisIsUsed)) {
        forward->removeFromList();
        checkAndClearBitMap(convertSizeToIndex(forward->getSize()) - 1);
        retBlock->size_and_flags += forward->size_and_flags & ~(0b1111);
    }
    // リストに追加
    retBlock->clearFlags(BoundaryTag::thisIsUsed);
    retBlock->getForward()->back_size_and_flags = retBlock->size_and_flags;
    retBlock->getForward()->clearFlags(BoundaryTag::backIsUsed);
    std::size_t index = convertSizeToIndex(retBlock->getSize()) - 1;
    tlsf_table[index].pushFront(retBlock);
    setBitMap(index);
}

oz::TLSFMemoryAllocator::BoundaryTag*
oz::TLSFMemoryAllocator::FreeList::popFront() {
    BoundaryTag* ret = this->link;
    this->link = ret->next_link;
    if (this->link != nullptr) {
        this->link->prev_link = ret->prev_link;
    }

    ret->next_link = nullptr;
    ret->prev_link = nullptr;

    return ret;
}

void oz::TLSFMemoryAllocator::FreeList::pushFront(BoundaryTag* target) {
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

void oz::TLSFMemoryAllocator::BoundaryTag::removeFromList() {
    this->prev_link->next_link = this->next_link;
    if (this->next_link != nullptr) {
        this->next_link->prev_link = this->prev_link;
    }
    this->next_link = nullptr;
    this->prev_link = nullptr;
}

void oz::TLSFMemoryAllocator::BoundaryTag::setFlags(std::uint64_t flagBits) {
    this->size_and_flags |= (flagBits & 0b1111);
}

void oz::TLSFMemoryAllocator::BoundaryTag::setSize(std::uint64_t size) {
    this->size_and_flags = (size & ~0b1111) | (this->size_and_flags & 0b1111);
}

void oz::TLSFMemoryAllocator::BoundaryTag::clearFlags(std::uint64_t flagBits) {
    this->size_and_flags &= ~(flagBits & 0b1111);
}

oz::TLSFMemoryAllocator::BoundaryTag*
oz::TLSFMemoryAllocator::BoundaryTag::getForward() {
    return reinterpret_cast<BoundaryTag*>(
        reinterpret_cast<std::uint8_t*>(this) + this->getSize());
}

oz::TLSFMemoryAllocator::BoundaryTag*
oz::TLSFMemoryAllocator::BoundaryTag::getBack() {
    return reinterpret_cast<BoundaryTag*>(
        reinterpret_cast<std::uint8_t*>(this) -
        (this->back_size_and_flags & ~(0b1111)));
}

bool oz::TLSFMemoryAllocator::BoundaryTag::test(std::uint64_t testFlagBits) {
    return (testFlagBits == (this->size_and_flags & testFlagBits));
}

std::size_t oz::TLSFMemoryAllocator::BoundaryTag::getSize() {
    return (this->size_and_flags & ~(0b1111));
}
