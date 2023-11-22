#pragma once

#include <new>
#include <cstddef>
#include "utils.hpp"
#include "IKernelMemoryAllocator.hpp"

[[nodiscard]] void* operator new(std::size_t size);
void operator delete(void* ptr) noexcept;//1
[[nodiscard]] void* operator new[](std::size_t size);
void operator delete[](void* ptr) noexcept;
void setMemoryAllocator(oz::IKernelMemoryAllocator* allocator);

// namespace oz{
//     template<std::size_t page_size, std::size_t pagePerChunk = 1, std::size_t LogBase2OfDivisionNum = 5>
//     class OzAllocator{
//         struct BoundaryTag {
//             std::uint64_t prev_size_and_flags;
//             std::uint64_t size_and_flags;
//             //63-4bit size,
//                 //tagPtr + size == nextTagPtr;
//                 //dataAreaSize == size - sizeof(prev_size_and_flags) - sizeof(size_and_flags) + sizeof(prev_size_and_flags);
//             //3bit undefined,
//             //2bit isLarge,
//             //1bit prevIsAllocated, 
//             //0bit thisIsAllocated
//             union {
//                 BoundaryTag* prev_link;
//                 std::uint8_t data;
//             };
//             BoundaryTag* next_link;
//         };
//         static_assert(sizeof(BoundaryTag) % 16 == 0);
//         static constexpr std::uint64_t divisionNum_first_level = oz::utils::getMSB(page_size*pagePerChunk) - LogBase2OfDivisionNum;
//         static constexpr std::uint64_t min_size_of_large_chunk = page_size*pagePerChunk - sizeof(BoundaryTag::prev_size_and_flags) - sizeof(BoundaryTag::size_and_flags);
//         static constexpr std::uint64_t min_size_of_block = 1 << LogBase2OfDivisionNum;
        
//         BoundaryTag* Categories[divisionNum_first_level][min_size_of_block];
//         std::uint8_t bitMap_exists_free_block_second_level[divisionNum_first_level];
//         std::uint64_t bitMap_exists_free_block_first_level;
//         public:
//         using allocatePage = void* (*)(std::size_t pageNum);
//         using freePage = void (*)(void* ptr, std::size_t pageNum);
//         private:
//         allocatePage palloc;
//         freePage pfree;

//         BoundaryTag* allocateChunk(){
//             BoundaryTag* ret = reinterpret_cast<BoundaryTag*>(palloc(pagePerChunk));
//             std::uint64_t size = page_size*pagePerChunk - sizeof(BoundaryTag::prev_size_and_flags) - sizeof(BoundaryTag::size_and_flags);
//             BoundaryTag* next = reinterpret_cast<BoundaryTag*>( &ret->data + size);
//             ret->size_and_flags = size | 0b0010;
//             next->size_and_flags = 0b0001;
//             return ret;
//         }

//         BoundaryTag* mallocLarge(std::size_t size){
//             std::size_t numPages = (size + sizeof(BoundaryTag::prev_size_and_flags) + page_size - 1)/page_size;
//             BoundaryTag* ret = reinterpret_cast<BoundaryTag*>(palloc(numPages));
//             ret->size_and_flags = (numPages << 4) | 0b0100;
//             return ret;
//         }

//         void freeLarge(BoundaryTag* returned){
//             std::size_t pageCount = returned->size_and_flags >> 4;
//             freePage(returned, pageCount);
//         }

//         //要素のないフリーリストに対して呼び出してはダメ
//         BoundaryTag* removeBlockFromList(std::size_t fli, std::size_t sli){
//             BoundaryTag* ret = Categories[fli][sli];
//             if(ret->next_link == nullptr){//０になるとき
//                 Categories[fli][sli] = nullptr;

//                 bitMap_exists_free_block_second_level[fli] =
//                     bitMap_exists_free_block_second_level[fli] & ~(0b0001 << sli);
//                 if(bitMap_exists_free_block_second_level[fli] == 0){
//                     bitMap_exists_free_block_first_level =
//                         bitMap_exists_free_block_first_level & ~(0b0001 << fli);
//                 }
//             }else{
//                 Categories[fli][sli] = ret->next_link;
//                 ret->next_link->prev_link = ret->prev_link;
//             }

//             ret->prev_link = nullptr;
//             ret->next_link = nullptr;

//             return ret;
//         }

//         BoundaryTag* removeBlockFromList(BoundaryTag* block){
//             if(static_cast<void*>(&Categories) <= static_cast<void*>(&block->prev_link->next_link) 
//                 && (static_cast<std::uint8_t*>(&Categories) + sizeof(Categories)) > (&block->prev_link->next_link)
//                 && block->next_link == nullptr){//0になるとき
//                 std::uint64_t block_size = block->size_and_flags & ~(0b00001111);

//                 std::size_t fli = oz::utils::getMSB(size) - LogBase2OfDivisionNum;
//                 std::uint64_t mask = 0;
//                 for(std::size_t i = 0; i < LogBase2OfDivisionNum; i++){
//                     mask = mask | (0b00000001 << i);
//                 }
//                 std::size_t sli = (size >> fli) & mask;

//                 bitMap_exists_free_block_second_level[fli] =
//                     bitMap_exists_free_block_second_level[fli] & ~(0b0001 << sli);
//                 if(bitMap_exists_free_block_second_level[fli] == 0){
//                     bitMap_exists_free_block_first_level =
//                         bitMap_exists_free_block_first_level & ~(0b0001 << fli);
//                 }

//                 block->prev_link == nullptr;

//             }else{

//                 block->prev_link->next_link = block->next_link;
//                 block->next_link->prev_link = block->prev_link;

//                 block->prev_link = nullptr;
//                 block->next_link = nullptr;

//             }
//             return block;
//         }

//         void addBlockToList(BoundaryTag* block){
//             std::uint64_t block_size = block->size_and_flags & ~(0b00001111);

//             std::size_t fli = oz::utils::getMSB(block_size) - LogBase2OfDivisionNum;
//             std::uint64_t mask = 0;
//             for(std::size_t i = 0; i < LogBase2OfDivisionNum; i++){
//                 mask = mask | (0b00000001 << i);
//             }
//             std::size_t sli = (block_size >> fli) & mask;

//             if(Categories[fli][sli] == nullptr){

//                 Categories[fli][sli] = block;
//                 block->next_link = nullptr;
//                 block->prev_link = reinterpret_cast<BoundaryTag*>(reinterpret_cast<std::uint8_t*>(&Categories[fli][sli]) - (sizeof(BoundaryTag::prev_link) + sizeof(BoundaryTag::size_and_flags) + sizeof(BoundaryTag::prev_size_and_flags)))//Categories[fli][sli]をBoundaryBlock::next_linkとみなす

//                 bitMap_exists_free_block_second_level[fli] =
//                     bitMap_exists_free_block_second_level[fli] | (0b0001 << sli);
                
//                 bitMap_exists_free_block_first_level =
//                     bitMap_exists_free_block_first_level | (0b0001 << fli);
                
//             }else{
//                 block->next_link = Categories[fli][sli];
//                 block->prev_link = block->next_link->prev_link;
//                 Categories[fli][sli] = block;
//                 block->next_link->prev_link = block;
//             }
//         }

//         void allocateBlock(BoundaryTag* block){
//             //allocatingBlockのthisIsAllocatedを立てる
//             block->size_and_flags = (block->size_and_flags) | 0b0001;
//             //次のBlockのprevIsAllocatedを立てる
//             BoundaryTag* nextBlock = reinterpret_cast<BoundaryTag*>(
//                     reinterpret_cast<std::uint8_t*>(&block->data)
//                     + (block->size_and_flags & ~(0b1111))
//                 );

//             nextBlock->size_and_flags = (nextBlock->size_and_flags) | 0b0010;
//         }

//         void freeBlock(BoundaryTag* block){
//             //blockのthisIsAllocatedを下す
//             block->size_and_flags = (block->size_and_flags) & ~(0b0001);
//             //次のBlockのprevIsAllocatedを下す
//             BoundaryTag* nextBlock = reinterpret_cast<BoundaryTag*>(
//                 reinterpret_cast<std::uint8_t>(block)
//                 + (block->size_and_flags & ~(0b1111))
//             );
//             nextBlock->size_and_flags = (nextBlock->size_and_flags) & ~(0b0010);
//             //次のBlockのprev_size_and_flags　に block の size_and_flagsを代入する
//             nextBlock->prev_size_and_flags = block->size_and_flags;

//         }

//     public:
//         OzAllocator(allocatePage allo, freePage fr){
//             palloc = allo;
//             pfree = fr;
//             for(std::size_t i = 0; i < (sizeof(Categories)/sizeof(Categories[0])); i++){
//                 for(std::size_t j = 0; j < (sizeof(Categories[i])/sizeof(Categories[i][0]))){
//                     Categories[i][j] = nullptr;
//                 }
//             }
//         }

//         void* malloc(std::size_t size){
//             size = (size + sizeof(BoundaryTag::size_and_flags) + 15) & ~(0b1111) ;//size を BoundaryTagを含む値に変換し16Byte境界に切り上げ
//             if(size > min_size_of_large_chunk){
//                 return &mallocLarge(size)->data;
//             }

//             std::size_t msb = oz::utils::getMSB(size);
//             std::size_t fli;
//             std::size_t sli;
//             if(msb < LogBase2OfDivisionNum){
//                 fli = 0;
//                 sli = 0;
//             }else{
//                 fli = msb - LogBase2OfDivisionNum;
//                 std::uint64_t mask = 0;
//                 for(std::size_t i = 0; i < LogBase2OfDivisionNum; i++){
//                     mask = mask | (0b00000001 << i);
//                 }
//                 sli = (size >> fli) & mask;
//             }

//             if(Categories[fli][sli] != nullptr){
//                 //フリーブロックリストから一つ取り出す
//                 BoundaryTag* allocatingBlock = removeBlockFromList(fli, sli);
//                 //allocatingBlockを確保状態にする
//                 allocateBlock(allocatingBlock);
//                 //ポインタを返す
//                 return &allocatingBlock->data;
//             }else{
//                 std::uint64_t mask_sli = ~(0) << sli;
//                 std::uint64_t bitMap_isExistsSecond = 
//                     bitMap_exists_free_block_second_level[fli] & mask_sli;
                
//                 BoundaryTag* allocatingBlock;

//                 //なかったらFirstLevelから探す
//                 if(bitMap_isExistsSecond == 0){
//                     std::uint64_t mask_fli = ~(0) << fli;
//                     std::uint64_t bitMap_isExistsFirst =
//                         bitMap_exists_free_block_first_level & mask_fli;
                    
//                     //空き容量がなかったら新しくページを要求する
//                     if(bitMap_isExistsFirst == 0){
//                         allocatingBlock = allocateChunk();
//                     }else{//あったらそれ
//                         allocatingBlock =
//                             removeBlockFromList(
//                                 oz::utils::getLSB(bitMap_isExistsFirst),
//                                 oz::utils::getLSB(bitMap_exists_free_block_second_level[oz::utils::getLSB(bitMap_isExistsFirst)]));
//                     }
//                 }else{//あったらそれ
//                     allocatingBlock = removeBlockFromList(fli, oz::utils::getLSB(bitMap_isExistsSecond));
//                 }

//                 if((allocatingBlock->size_and_flags & ~(0b00001111)) >= size + min_size_of_block){//分割可能
//                     BoundaryTag* forkedBlock = reinterpret_cast<BoundaryTag*>(reinterpret_cast<std::uint8_t*>(allocatingBlock) + size);

//                     std::uint64_t forkedBlockSize = (allocatingBlock->size_and_flags & ~(0b00001111)) - size;

//                     allocatingBlock->size_and_flags = size | (allocatingBlock->size_and_flags & 0b00001111);
//                     forkedBlock->size_and_flags = forkedBlockSize;
//                     freeBlock(forkedBlock);
//                     addBlockToList(forkedBlock);
//                 }

//                 allocateBlock(allocatingBlock);
//                 return &allocatingBlock->data;
//             }

//         }

//         void free(void* ptr){
//             //BoundaryTagに変換
//             BoundaryTag* retBlock = reinterpret_cast<BoundaryTag*>(reinterpret_cast<std::uint8_t*>(ptr) - (sizeof(BoundaryTag::size_and_flags) + sizeof(BoundaryTag::prev_size_and_flags)));
//             //もしisLargeならfreeLarge
//             if((retBlock->size_and_flags & 0b0100) != 0){
//                 freeLarge(retBlock);
//                 return;
//             }
//             //前者が未使用ならマージ
//             if((retBlock->size_and_flags & 0b0010) == 0){
//                 BoundaryTag* back = reinterpret_cast<BoundaryTag*>(
//                     reinterpret_cast<std::uint8_t*>(retBlock)
//                     - (retBlock->prev_size_and_flags & ~(0b1111))
//                 );
//                 removeBlockFromList(back);
//                 back->size_and_flags += retBlock->size_and_flags & ~(0b1111);
//                 retBlock = back;
//             }
//             //後者が未使用ならマージ
//             BoundaryTag* forward = reinterpret_cast<BoundaryTag*>(
//                 reinterpret_cast<std::uint8_t*>(retBlock)
//                 + (retBlock->size_and_flags & ~(0b1111))
//             );
//             if((forward->size_and_flags & 0b0001) == 0){
//                 retBlock->size_and_flags += forward->size_and_flags & ~(0b1111);
//             }
//             //リストに追加
//             freeBlock(retBlock);
//             addBlockToList(retBlock);
//         }

//     };
// }