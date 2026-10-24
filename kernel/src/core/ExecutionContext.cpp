#include "core/ExecutionContext.hpp"
#include "core/Baton.hpp"

namespace oz {

ExecutionContext::ExecutionContext(std::uint64_t id, void* stack, std::uint64_t size, PageBlock<> block, std::uint8_t level,
                                   PhysicalAddress<PageTable> pml4_phys)
    : id(id), rsp(reinterpret_cast<std::uint8_t*>(stack) + size),
      stack_base(stack), stack_size(size), stack_block(block), frame_level(level),
      state(ThreadState::Ready), pml4_phys(pml4_phys) {
}

ExecutionContext::~ExecutionContext() {
    if (baton && baton->thread == this) {
        baton->thread = nullptr;
        baton = nullptr;
    }
}

bool ExecutionContext::has_baton() const {
    return baton != nullptr && baton->has_budget();
}

} // namespace oz
