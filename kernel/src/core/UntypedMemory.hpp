#pragma once
#include "core/Capability.hpp"
#include "core/Frame.hpp"
#include "core/Thread.hpp"
#include "core/TCB.hpp"
#include "core/CSpaceCap.hpp"
#include "core/Channel.hpp"
#include "core/PageTableObject.hpp"
#include "memory/Address.hpp"
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <new>

namespace oz {

class UntypedMemory : public KernelObject {
public:
    using FreeFn = void (*)(void* cleanup_data);

private:
    std::uintptr_t phys_base_{0};
    std::size_t size_{0};
    std::size_t watermark_{0};
    bool is_device_{false};
    void* cleanup_data_{nullptr};
    FreeFn free_fn_{nullptr};

public:
    void* allocateBytes(std::size_t bytes, std::size_t alignment, std::uintptr_t& out_phys) {
        std::size_t aligned_watermark = (watermark_ + (alignment - 1)) & ~(alignment - 1);
        if (aligned_watermark + bytes > size_) {
            return nullptr;
        }
        out_phys = phys_base_ + aligned_watermark;
        watermark_ = aligned_watermark + bytes;
        return reinterpret_cast<void*>(out_phys + DIRECT_MAP_OFFSET);
    }

    UntypedMemory(std::uintptr_t phys_base, std::size_t size,
                  void* cleanup_data = nullptr, FreeFn free_fn = nullptr,
                  bool is_device = false, bool untyped = false)
        : KernelObject(ObjectType::UntypedMemory, untyped),
          phys_base_(phys_base), size_(size), watermark_(0),
          is_device_(is_device), cleanup_data_(cleanup_data), free_fn_(free_fn) {}

    ~UntypedMemory() override {
        if (free_fn_ && cleanup_data_) {
            free_fn_(cleanup_data_);
        }
    }

    std::uintptr_t physicalBase() const { return phys_base_; }
    std::size_t totalBytes() const { return size_; }
    std::size_t remainingBytes() const {
        return (watermark_ < size_) ? (size_ - watermark_) : 0;
    }
    std::size_t remainingPages() const { return remainingBytes() / 4096; }

    void revokeRecursive(KernelObject* node) {
        if (!node) return;
        while (node->cdtFirstChild()) {
            revokeRecursive(node->cdtFirstChild());
        }
        node->markRevoked();
        if (node->objectType() == ObjectType::ChannelEndpoint) {
            auto* ep = static_cast<ChannelEndpoint*>(node);
            ep->on_peer_closed();
        } else if (node->objectType() == ObjectType::TCB) {
            auto* tcb = static_cast<TCB*>(node);
            if (tcb->getThread()) {
                tcb->getThread()->state = ThreadState::Dead;
            }
        }
        this->removeChild(node);
        node->release();
    }

    void revoke() {
        while (cdt_first_child_) {
            revokeRecursive(cdt_first_child_);
        }
        watermark_ = 0;
    }

    template <typename T, typename... Args>
    T* allocateObject(Args&&... args) {
        std::uintptr_t obj_phys = 0;
        void* obj_mem = allocateBytes(sizeof(T), alignof(T), obj_phys);
        if (!obj_mem) return nullptr;
        T* obj = new (obj_mem) T(static_cast<Args&&>(args)...);
        if constexpr (std::is_base_of_v<KernelObject, T>) {
            obj->setUntypedBacked(true);
            this->addChild(obj);
        }
        return obj;
    }

    // Retype into a Frame (user-mapped physical memory)
    Frame* retypeFrame(std::size_t num_pages) {
        if (num_pages == 0) return nullptr;

        // 1. Allocate metadata (Frame struct)
        std::uintptr_t meta_phys = 0;
        void* meta_mem = allocateBytes(sizeof(Frame), alignof(Frame), meta_phys);
        if (!meta_mem) return nullptr;

        // 2. Allocate user physical pages (4096-aligned)
        std::uintptr_t frame_phys = 0;
        void* frame_mem = allocateBytes(num_pages * 4096, 4096, frame_phys);
        if (!frame_mem) {
            watermark_ = meta_phys - phys_base_;
            return nullptr;
        }

        // Construct Frame object with placement new
        auto* frame = new (meta_mem) Frame(frame_phys, num_pages * 4096, is_device_, nullptr, nullptr, true);
        this->addChild(frame);
        return frame;
    }

    // Retype into a PageTable (x86-64 intermediate paging structure: 4096 bytes)
    PageTableObject* retypePageTable() {
        // 1. Allocate metadata (PageTableObject struct)
        std::uintptr_t meta_phys = 0;
        void* meta_mem = allocateBytes(sizeof(PageTableObject), alignof(PageTableObject), meta_phys);
        if (!meta_mem) return nullptr;

        // 2. Allocate 4096-aligned physical page for table entries
        std::uintptr_t pt_phys = 0;
        void* pt_mem = allocateBytes(4096, 4096, pt_phys);
        if (!pt_mem) {
            watermark_ = meta_phys - phys_base_;
            return nullptr;
        }

        auto* pt = new (meta_mem) PageTableObject(pt_phys, true);
        this->addChild(pt);
        return pt;
    }

    // Retype into a TCB (Thread Control Block) with integrated 8KB kernel stack
    TCB* retypeTCB(void (*queue_thread_fn)(Thread*) = nullptr) {
        static constexpr std::size_t TCB_BLOCK_SIZE       = 8192;
        static constexpr std::size_t TCB_OBJECT_OFFSET    = 0;
        static constexpr std::size_t TCB_MAX_SIZE         = 128;
        static constexpr std::size_t THREAD_OBJECT_OFFSET = 128;
        static constexpr std::size_t THREAD_MAX_SIZE      = 512 - 128; // 384 bytes
        static constexpr std::size_t STACK_OFFSET         = 512;
        static constexpr std::size_t STACK_SIZE           = TCB_BLOCK_SIZE - STACK_OFFSET; // 7680 bytes

        static_assert(sizeof(TCB) <= TCB_MAX_SIZE, "TCB size exceeds allocation slot");
        static_assert(sizeof(Thread) <= THREAD_MAX_SIZE, "Thread size exceeds allocation slot");

        // Allocate 8192 bytes (4096-aligned)
        std::uintptr_t block_phys = 0;
        void* block_mem = allocateBytes(TCB_BLOCK_SIZE, 4096, block_phys);
        if (!block_mem) return nullptr;

        // Memory layout within this 8KB block:
        //   [0 .. 64): TCB object
        //   [64 .. 512): Thread object
        //   [512 .. 8192): Kernel stack (7680 bytes)
        std::uint8_t* base = reinterpret_cast<std::uint8_t*>(block_mem);
        void* tcb_mem = base + TCB_OBJECT_OFFSET;
        void* thread_mem = base + THREAD_OBJECT_OFFSET;
        void* stack_base = base + STACK_OFFSET;
        std::size_t stack_size = STACK_SIZE;

        Thread* thread = new (thread_mem) Thread(
            0, stack_base, stack_size, PageBlock<>{nullptr}, 0
        );
        thread->is_untyped_backed = true;
        thread->state = ThreadState::Blocked;

        TCB* tcb = new (tcb_mem) TCB(thread, true, queue_thread_fn, true);
        this->addChild(tcb);
        return tcb;
    }

    // Retype into a CSpace (256 capability slots = 4096 bytes)
    CSpaceCap* retypeCSpace() {
        std::uintptr_t meta_phys = 0;
        void* meta_mem = allocateBytes(sizeof(CSpaceCap), alignof(CSpaceCap), meta_phys);
        if (!meta_mem) return nullptr;

        std::uintptr_t cs_phys = 0;
        void* cs_mem = allocateBytes(sizeof(CSpace), 4096, cs_phys);
        if (!cs_mem) {
            watermark_ = meta_phys - phys_base_;
            return nullptr;
        }

        CSpace* cs = new (cs_mem) CSpace();
        auto* cscap = new (meta_mem) CSpaceCap(cs, false, true);
        this->addChild(cscap);
        return cscap;
    }

    // Retype into a ChannelEndpoint pair
    bool retypeChannelPair(ChannelEndpoint** out0, ChannelEndpoint** out1) {
        if (!out0 || !out1) return false;

        std::uintptr_t phys0 = 0;
        void* mem0 = allocateBytes(sizeof(ChannelEndpoint), alignof(ChannelEndpoint), phys0);
        if (!mem0) return false;

        std::uintptr_t phys1 = 0;
        void* mem1 = allocateBytes(sizeof(ChannelEndpoint), alignof(ChannelEndpoint), phys1);
        if (!mem1) {
            watermark_ = phys0 - phys_base_;
            return false;
        }

        ChannelEndpoint* ep0 = new (mem0) ChannelEndpoint(true);
        ChannelEndpoint* ep1 = new (mem1) ChannelEndpoint(true);
        ep0->set_peer(ep1);
        ep1->set_peer(ep0);

        this->addChild(ep0);
        this->addChild(ep1);

        *out0 = ep0;
        *out1 = ep1;
        return true;
    }
};

} // namespace oz
