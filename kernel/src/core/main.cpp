#include <cstdint>
#include <new>

#include "memory/Address.hpp"
#include "memory/FrameManager.hpp"
#include "core/Kernel.hpp"
#include "memory/KernelMemoryAllocator.hpp"
#include "memory/PageTable.hpp"
#include "core/bootStructures.hpp"
#include "utils/utils.hpp"
#include "hardware/x86_64.hpp"

// Early boot page tables
extern "C" {
alignas(4096) volatile oz::PageTable boot_pml4;
alignas(4096) volatile oz::PageTable boot_pdpt_low;
alignas(4096) volatile oz::PageTable boot_pdpt_direct;
alignas(4096) volatile oz::PageTable boot_pdpt_direct_2;
alignas(4096) volatile oz::PageTable boot_pdpt_kernel;
alignas(4096) volatile oz::PageTable boot_pd_kernel;
}

alignas(16) static std::uint8_t stack[1024 * 1024 * 2];

namespace oz {
PageTable* getMasterPML4() {
    return const_cast<PageTable*>(&boot_pml4);
}
}

extern "C" void kernel_main(oz_boot::PlatformInfo* platformInfo);

extern "C" [[gnu::section(".boot")]] void init(oz_boot::PlatformInfo* platformInfo) {
    volatile oz::PageTable* pml4;
    volatile oz::PageTable* pdpt_low;
    volatile oz::PageTable* pdpt_direct;
    volatile oz::PageTable* pdpt_direct_2;
    volatile oz::PageTable* pdpt_kernel;
    volatile oz::PageTable* pd_kernel;

    // Obtain the physical addresses of boot page tables using RIP-relative addressing
    __asm__ volatile(
        "lea boot_pml4(%%rip), %0\n"
        "lea boot_pdpt_low(%%rip), %1\n"
        "lea boot_pdpt_direct(%%rip), %2\n"
        "lea boot_pdpt_direct_2(%%rip), %3\n"
        "lea boot_pdpt_kernel(%%rip), %4\n"
        "lea boot_pd_kernel(%%rip), %5\n"
        : "=r"(pml4), "=r"(pdpt_low), "=r"(pdpt_direct), "=r"(pdpt_direct_2), "=r"(pdpt_kernel), "=r"(pd_kernel)
    );

    // 1. Low Identity Map (PML4[0]) - First 4GB with 1GB huge pages
    (*pml4)[0].set(reinterpret_cast<std::uintptr_t>(pdpt_low), oz::PageFlags::Present | oz::PageFlags::Writable);
    for (std::uint64_t i = 0; i < 4; ++i) {
        (*pdpt_low)[i].set(i * 0x40000000ULL, oz::PageFlags::Present | oz::PageFlags::Writable | oz::PageFlags::HugePage);
    }

    // 2. Direct Map (PML4[272] = 0xFFFF880000000000) - 512GB with 1GB huge pages
    (*pml4)[272].set(reinterpret_cast<std::uintptr_t>(pdpt_direct), oz::PageFlags::Present | oz::PageFlags::Writable);
    for (std::uint64_t i = 0; i < 512; ++i) {
        (*pdpt_direct)[i].set(i * 0x40000000ULL, oz::PageFlags::Present | oz::PageFlags::Writable | oz::PageFlags::HugePage);
    }
    
    // 2.5. Direct Map part 2 (PML4[273] = 0xFFFF888000000000) - 512GB with 1GB huge pages
    (*pml4)[273].set(reinterpret_cast<std::uintptr_t>(pdpt_direct_2), oz::PageFlags::Present | oz::PageFlags::Writable);
    for (std::uint64_t i = 0; i < 512; ++i) {
        (*pdpt_direct_2)[i].set((i + 512) * 0x40000000ULL, oz::PageFlags::Present | oz::PageFlags::Writable | oz::PageFlags::HugePage);
    }

    // 3. Higher-Half Kernel Map (PML4[511] = 0xFFFFFFFF80000000) - 1GB with 2MB huge pages
    (*pml4)[511].set(reinterpret_cast<std::uintptr_t>(pdpt_kernel), oz::PageFlags::Present | oz::PageFlags::Writable);
    (*pdpt_kernel)[510].set(reinterpret_cast<std::uintptr_t>(pd_kernel), oz::PageFlags::Present | oz::PageFlags::Writable);
    for (std::uint64_t i = 0; i < 512; ++i) {
        (*pd_kernel)[i].set(i * 0x200000ULL, oz::PageFlags::Present | oz::PageFlags::Writable | oz::PageFlags::HugePage);
    }

    // 4. Load CR3 with physical address of boot_pml4
    std::uint64_t pml4_phys = reinterpret_cast<std::uint64_t>(pml4);
    __asm__ volatile("movq %0, %%cr3" :: "r"(pml4_phys) : "memory");

    // 5. Switch to higher-half stack and jump to kernel_main
    std::uint8_t* stackBase = stack + sizeof(stack);
    
    // x86_64 System V ABI requires RSP to be 16-byte aligned BEFORE a call.
    // A call pushes an 8-byte return address, so on function entry, RSP % 16 == 8.
    // Since we are using JMP instead of CALL, we must manually subtract 8 to simulate the pushed return address.
    stackBase -= 8;
    
    void (*entry)(oz_boot::PlatformInfo*) = &kernel_main;

    __asm__ volatile(
        "movq %0, %%rsp\n"
        "movq %1, %%rdi\n"
        "jmp *%2\n"
        :: "r"(stackBase), "r"(platformInfo), "r"(entry)
        : "memory"
    );
}

class BootInfoProvider : public oz::PhysicalAddressProvider {
public:
    template <typename T>
    static oz::PhysicalAddress<T> getPhys(T* p) {
        return createPhysicalAddress<T>(reinterpret_cast<std::uintptr_t>(p));
    }
};

struct Settings {
    using FrameManager = oz::x86_64::FrameManager;
    template <typename Accessor>
    using KernelMemoryAllocatorFunctor = oz::TLSFMemoryAllocator<Accessor>;
};

extern "C" void kernel_main(oz_boot::PlatformInfo* platformInfoPhys) {
    // 6. Convert PlatformInfo pointer to direct map virtual address
    oz_boot::PlatformInfo* platformInfo = oz::phys_to_virt(BootInfoProvider::getPhys(platformInfoPhys));

    // 7. Unmap lower identity mapping
    boot_pml4[0].clear();
    std::uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0; movq %0, %%cr3" : "=r"(cr3) :: "memory");

    // 8. Convert PlatformInfo member physical pointers to direct map virtual addresses
    platformInfo->frame_buffer_base = oz::phys_to_virt(BootInfoProvider::getPhys(static_cast<std::uint8_t*>(platformInfo->frame_buffer_base)));
    platformInfo->memory_map.buffer = oz::phys_to_virt(BootInfoProvider::getPhys(static_cast<std::uint8_t*>(platformInfo->memory_map.buffer)));
    if (platformInfo->RSDP) {
        platformInfo->RSDP = oz::phys_to_virt(BootInfoProvider::getPhys(platformInfo->RSDP));
    }

    // 9. Initialize architecture specifics
    oz::x86_64::enableSSE();
    oz::x86_64::initGDTR();
    oz::x86_64::initIDTR();

    // 10. Initialize and run Kernel
    // setKernelPtr(static_cast<void*>(&k));
    using KStorage = oz::KernelStorage<Settings>;
    new (&KStorage::kernel_storage.kernel) KStorage::Kernel{platformInfo};
    // reinterpret_cast<KStorage::Kernel*>(&k)->run();
    KStorage::kernel_storage.kernel.run();

    while (1) __asm__ volatile("hlt");
}