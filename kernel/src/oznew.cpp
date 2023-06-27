#include "oznew.hpp"
// #include "oznew.hpp"

// #include <cstdint>

// namespace {

//     //sizeメンバ
//     //一桁目 prev_in_use
//     //二桁目 self_in_use

//     struct Chunk
//     {
//         std::uint64_t prev_size_or_prev_user_data;
//         std::uint64_t size;
//     };

//     struct FreeChunk
//     {
//         std::uint64_t prev_size_or_prev_user_data;
//         std::uint64_t size;
//         FreeChunk* back;
//         FreeChunk* forward;
//     };

//     struct AllocatedChunk
//     {
//         std::uint64_t prev_size_or_prev_user_data;
//         std::uint64_t size;
//         std::uint64_t user_data[];
//     };

//     std::uint64_t heap[1024];

//     FreeChunk* freeListHead;

//     void setUpMemoryManager(){
//         FreeChunk* freeChunk = reinterpret_cast<FreeChunk*>(&heap);
//         freeChunk->size = (sizeof(heap) - sizeof(std::uint64_t)) | 0b01;
//         freeChunk->back = freeChunk;
//         freeChunk->forward = freeChunk;

//         freeListHead = freeChunk;
//     }

//     void* ozmalloc(std::uint64_t size){
//         //サイズを8バイト単位に切り上げる
//         size = (size + 0b111) & ~0b111;
//         FreeChunk* beforeFreeListHead = freeListHead;
//         do{

//             if((freeListHead->size & ~0b111) == sizeof(std::uint64_t) + size){
//                 //headをFreeListから外す
//                 freeListHead->back->forward = freeListHead->forward;
//                 freeListHead->forward->back = freeListHead->back;
                
//                 //次のチャンクのprev_in_useフラグを立てる
//                 Chunk* nextChunk = reinterpret_cast<Chunk*>(reinterpret_cast<std::uint8_t*>(freeListHead) + (freeListHead->size & ~0b111));
//                 nextChunk->size = (nextChunk->size | 0b1);

//                 //allocatedChunkのself_in_useフラグを立てる
//                 AllocatedChunk* allocatedChunk = reinterpret_cast<AllocatedChunk*>(freeListHead);
//                 allocatedChunk->size = allocatedChunk->size | 0b10;
//                 freeListHead = freeListHead->back;

//                 return &allocatedChunk->user_data;
//             }else if((freeListHead->size & ~0b111) >= sizeof(FreeChunk) + sizeof(std::uint64_t) + size){
//                 //headのsizeを確保するチャンクのサイズ分減らす
//                 freeListHead->size = freeListHead->size - sizeof(std::uint64_t) - size;

//                 //確保するチャンクの設定をする
//                 AllocatedChunk* allocatedChunk = reinterpret_cast<AllocatedChunk*>(reinterpret_cast<std::uint8_t*>(freeListHead) + (freeListHead->size & ~0b111));
//                 allocatedChunk->prev_size_or_prev_user_data = freeListHead->size;
//                 allocatedChunk->size = (sizeof(std::uint64_t) + size) | 0b10;//prev_in_use=0, self_in_use=1

//                 //allocatedChunkの次のチャンクのprev_in_useフラグを立てる
//                 Chunk* nextChunk = reinterpret_cast<Chunk*>(reinterpret_cast<std::uint8_t*>(allocatedChunk) + (allocatedChunk->size & ~0b111));
//                 nextChunk->size = nextChunk->size | 0b1;

//                 return &allocatedChunk->user_data;
//             }

//             freeListHead = freeListHead->forward;

//         }while(beforeFreeListHead != freeListHead);

//         return nullptr;
//     }

//     void ozfree(void* ptr){
//         FreeChunk* releasingChunk = reinterpret_cast<FreeChunk*>(reinterpret_cast<std::uint8_t*>(ptr) - sizeof(AllocatedChunk));
//         Chunk* nextChunk = reinterpret_cast<Chunk*>(reinterpret_cast<std::uint8_t*>(releasingChunk) + (releasingChunk->size & ~0b111));
        
//         if((nextChunk->size & 0b10) == 0){//nextChunkのself_in_useフラグが立っていないとき
//             FreeChunk* nextChunkF = reinterpret_cast<FreeChunk*>(nextChunk);
//             //nextChunkをfreeListから抜く
//             nextChunkF->back->forward = nextChunkF->forward;
//             nextChunkF->forward->back = nextChunkF->back;

//             //nextChunkをreleasingChunkに併合する
//             releasingChunk->size += nextChunkF->size & ~0b111;

//             nextChunk = reinterpret_cast<Chunk*>(reinterpret_cast<std::uint8_t*>(releasingChunk) + (releasingChunk->size & ~0b111));
//         }

//         if((releasingChunk->size & 0b1) == 0){//prev_in_useフラグが立っていないとき
//             FreeChunk* prevChunk = reinterpret_cast<FreeChunk*>(reinterpret_cast<std::uint8_t*>(releasingChunk) - (releasingChunk->prev_size_or_prev_user_data & ~0b111));
//             //releasingChunkをprevChunkに併合する
//             prevChunk->size += releasingChunk->size & ~0b111;

//             //nextChunkのprev_in_useフラグを下しprev_sizeをセットする
//             nextChunk->size = nextChunk->size & ~0b1;
//             nextChunk->prev_size_or_prev_user_data = prevChunk->size;

//         }else{

//             //releasingChunkをfreeListに追加する
//             releasingChunk->back = freeListHead;
//             releasingChunk->forward = freeListHead->forward;
//             releasingChunk->back->forward = releasingChunk;
//             releasingChunk->forward->back = releasingChunk;

//             //releasingChunkのself_in_useフラグを下す
//             releasingChunk->size = releasingChunk->size & ~0b10;

//             //nextChunkのprev_in_useフラグを下しprev_sizeをセットする
//             nextChunk->size = nextChunk->size & ~0b1;
//             nextChunk->prev_size_or_prev_user_data = releasingChunk->size;
//         }
//     }
// }

// [[nodiscard]] void* operator new(std::size_t size)
// {
//     return ozmalloc(size);
// }

// [[nodiscard]] void* operator new(std::size_t size, std::align_val_t alignment)
// {
//     return operator new(size);
// }

// [[nodiscard]] void* operator new(std::size_t size, const ozstd::nothrow_t&)noexcept
// {
//     // try{
//         return operator new(size);
//     // }catch(...){
//     //     return nullptr;
//     // }
// }

// [[nodiscard]] void* operator new(std::size_t size, std::align_val_t alignment, const ozstd::nothrow_t&)noexcept
// {
//     // try{
//         return operator new(size, alignment);
//     // }catch(...){
//     //     return nullptr;
//     // }
// }

// [[nodiscard]] void* operator new(std::size_t size, void* ptr)noexcept
// {
//     return ptr;
// }

// void operator delete(void* ptr) noexcept
// {
//     ozfree(ptr);
// }
// void operator delete(void* ptr, std::size_t size)noexcept
// {
//     ozfree(ptr);
// }


// void operator delete(void* ptr, std::align_val_t alignment)noexcept
// {
//     ozfree(ptr);
// }
// void operator delete(void* ptr, std::size_t size, std::align_val_t
// alignment)noexcept
// {
//     ozfree(ptr);
// }
// void operator delete(void* ptr, const ozstd::nothrow_t&)noexcept
// {
//     ozfree(ptr);
// }
// void operator delete(void* ptr, std::align_val_t alignment, const
// ozstd::nothrow_t&) noexcept
// {
//     ozfree(ptr);
// }

namespace {
    oz::IKernelMemoryAllocator* allocator;
}

void* operator new(std::size_t size) {
    return allocator->malloc(size);    
}
void operator delete(void* ptr) noexcept {
    allocator->free(ptr);
}
void setMemoryAllocator(oz::IKernelMemoryAllocator* al) {
    allocator = al;
}
// void operator delete(void* ptr, void*)noexcept{}