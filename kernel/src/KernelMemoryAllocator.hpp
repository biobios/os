#pragma once
#include <array>
#include "IKernelMemoryAllocator.hpp"
#include "IFrameManager.hpp"

namespace oz{

    class TLSFMemoryAllocator : public IKernelMemoryAllocator{
    private:
        struct BoundaryTag {
            union {
                std::uint64_t back_size_and_flags;
                FrameInfo* frameInfoPtr;//isLargeが立っているとき有効
            };
            std::uint64_t size_and_flags;
            //63-4bit size,
                //tagPtr + size == nextTagPtr;
                //dataAreaSize == size - sizeof(prev_size_and_flags) - sizeof(size_and_flags) + sizeof(prev_size_and_flags);
            //3bit undefined,
            //2bit isLarge,
            //1bit backIsAllocated, 
            //0bit thisIsAllocated
            union {
                BoundaryTag* prev_link;
                std::uint8_t data;
            };
            BoundaryTag* next_link;
            static constexpr std::uint64_t thisIsUsed = 0b0001;
            static constexpr std::uint64_t backIsUsed = 0b0010;
            static constexpr std::uint64_t isLarge = 0b0100;
            /// @brief FreeListに載っていないものに対して呼び出してはダメ
            void removeFromList();
            void setFlags(std::uint64_t flagBits);
            void setSize(std::uint64_t size);
            /// @brief flagBitsの立っているビットをクリアする
            /// @param flagBits 
            void clearFlags(std::uint64_t flagBits);
            BoundaryTag* getForward();
            BoundaryTag* getBack();
            /// @brief testFlagsBitsの立っているビットに対応するフラグがすべて立っているとtrue
            ///        それ以外はfalse
            /// @param testFlagBits 4bit幅である必要がある
            /// @return 
            bool test(std::uint64_t testFlagBits);
            std::size_t getSize();
        };
        static_assert(sizeof(BoundaryTag) == 32, "");
        struct FreeList {
            BoundaryTag* link;
            BoundaryTag* popFront();
            void pushFront(BoundaryTag* target);
        };
        /// @brief TwoLevelIndex
        using TLI = std::array<std::size_t, 2>;
        static constexpr std::size_t size_align_2order = 4;
        FreeList* tlsf_table;
        std::uint32_t bitMap_not_empty_freelist_second_level[64] = {};
        std::uint64_t bitMap_not_empty_freelist_first_level = 0;
        std::size_t max_log2_SLI;
        std::size_t max_FLI;
        std::size_t framePerChunk;
        std::size_t max_size_of_block;
        std::size_t min_size_of_block;
        IFrameManager* frameManager;
        std::size_t convertTLItoLinearIndex(TLI& tli);
        TLI convertLinearIndexToTLI(std::size_t linear);
        TLI convertSizeToTLI(std::size_t size);
        std::size_t convertSizeToIndex(std::size_t size);
        BoundaryTag* newBlock();
        BoundaryTag* mallocLarge(std::size_t size);
        void freeLarge(BoundaryTag* returnedBlock);
        /// @brief 指定したFreeListが空ならbitMapの対応するflagをクリアする
        /// @param linearIndex 
        void checkAndClearBitMap(std::size_t linearIndex);
        /// @brief 指定したFreeListが空ならbitMapの対応するflagをクリアする
        /// @param fli 
        /// @param sli 
        void checkAndClearBitMap(TLI& tli);
        void setBitMap(std::size_t linearIndex);
        void setBitMap(TLI& tli);
    public:
        TLSFMemoryAllocator(IFrameManager* fm, std::size_t framePerChunk = 1, std::size_t Log2_SLI = 5);
        void* malloc(std::size_t size) override;
        void free(void* ptr) override;
    };
}