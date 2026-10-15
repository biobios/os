#pragma once

#include <cstdint>
#include <memory>
#include "core/Thread.hpp"
#include "memory/AddressSpace.hpp"

#include "core/Capability.hpp"

namespace oz {

template <typename KernelAccessor>
class Process {
public:
    using AddressSpaceType = ProcessAddressSpace<
        KernelAccessor,
        x86_64AddressSpaceContext<KernelAccessor>
    >;

    std::uint64_t pid;
    AddressSpaceType address_space;
    Thread* main_thread;
    std::uint64_t entry_point;
    CSpace* cspace{nullptr};
    std::uint64_t mmap_bump = 0x100000000000;

    Process(std::uint64_t id, AddressSpaceType as, Thread* thread, std::uint64_t entry = 0, CSpace* cs = nullptr)
        : pid(id), address_space(std::move(as)), main_thread(thread), entry_point(entry), cspace(cs) {}

    void activate() {
        address_space.activate();
    }
};

extern "C" void enter_user_mode(std::uint64_t rip, std::uint64_t user_rsp, std::uint64_t rflags);

} // namespace oz
