#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bootStructures.hpp"
#include "core/Scheduler.hpp"
#include "core/Thread.hpp"
#include "memory/AddressSpace.hpp"
#include "memory/FrameManager.hpp"
#include "memory/PageTable.hpp"
#include "memory/PageTableManager.hpp"

namespace oz {

PageTable* getMasterPML4();
class UntypedMemory;

class Kernel : public PhysicalAddressProvider {
public:
    static Kernel& instance();

    explicit Kernel(oz_boot::PlatformInfo* platformInfo);
    void run();

    KernelAddressSpace& getKernelSpace() { return kernel_space; }
    PageTableManager& getPageTableManager() { return pt_manager; }
    x86_64::FrameManager& getFrameManager() { return fm; }
    Scheduler& getScheduler() { return scheduler; }

private:
    struct BootDevices {
        std::uint64_t ahci_abar_phys{0};
        std::uint64_t xhci_bar_phys{0};
    };

    void initSyscallAndStubs();
    BootDevices probeBootDevices();
    const oz_boot::BootModuleInfo* findBootModule(const char* name);
    Thread* loadRootServerFromInitrd(UntypedMemory& boot_untyped, const oz_boot::BootModuleInfo* initrd_file);
    Thread* loadProcessFromElfBuffer(UntypedMemory& boot_untyped, const std::uint8_t* file_buf, std::size_t file_size);
    void setupRootServerCaps(Thread* init_thread, UntypedMemory& boot_untyped, const BootDevices& devices, const oz_boot::BootModuleInfo* initrd_file);
    void setupIdleThread();

    x86_64::FrameManager fm;
    PageTableManager pt_manager;
    KernelAddressSpace kernel_space;
    Scheduler scheduler;
    oz_boot::PlatformInfo* platform_info_;
};

}  // namespace oz
