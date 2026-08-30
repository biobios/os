#pragma once

#include <cstdint>
#include <memory>
#include "core/Thread.hpp"
#include "memory/AddressSpace.hpp"

namespace oz {

template <typename KernelAccessor>
class Process {
public:
    using AddressSpaceType = ProcessAddressSpace<
        KernelAccessor,
        x86_64AddressSpaceContext<KernelAccessor>
    >;

    std::uint64_t pid;
    std::unique_ptr<AddressSpaceType> address_space;
    Thread* main_thread;

    Process(std::uint64_t id, std::unique_ptr<AddressSpaceType> as, Thread* thread)
        : pid(id), address_space(std::move(as)), main_thread(thread) {}

    void activate() {
        if (address_space) {
            address_space->activate();
        }
    }
};

extern "C" void enter_user_mode(std::uint64_t rip, std::uint64_t user_rsp);

} // namespace oz
