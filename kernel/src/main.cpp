#include <cstdint>
#include <new>

#include "Address.hpp"
#include "Kernel.hpp"
#include "bootStructures.hpp"
#include "utils.hpp"
#include "x86_64.hpp"

// Early boot page tables
extern "C" {
alignas(4096) volatile std::uint64_t boot_pml4[512];
alignas(4096) volatile std::uint64_t boot_pdpt_low[512];
alignas(4096) volatile std::uint64_t boot_pdpt_direct[512];
alignas(4096) volatile std::uint64_t boot_pdpt_kernel[512];
alignas(4096) volatile std::uint64_t boot_pd_kernel[512];
}

alignas(oz::Kernel) static std::uint8_t k[sizeof(oz::Kernel)];
alignas(16) static std::uint8_t stack[1024 * 1024 * 2];

namespace oz {
std::uint64_t* getMasterPML4() {
    return const_cast<std::uint64_t*>(boot_pml4);
}
}

extern "C" void kernel_main(oz_boot::PlatformInfo* platformInfo);

extern "C" [[gnu::section(".boot")]] void init(oz_boot::PlatformInfo* platformInfo) {
    volatile std::uint64_t* pml4;
    volatile std::uint64_t* pdpt_low;
    volatile std::uint64_t* pdpt_direct;
    volatile std::uint64_t* pdpt_kernel;
    volatile std::uint64_t* pd_kernel;

    // Obtain the physical addresses of boot page tables using RIP-relative addressing
    __asm__ volatile(
        "lea boot_pml4(%%rip), %0\n"
        "lea boot_pdpt_low(%%rip), %1\n"
        "lea boot_pdpt_direct(%%rip), %2\n"
        "lea boot_pdpt_kernel(%%rip), %3\n"
        "lea boot_pd_kernel(%%rip), %4\n"
        : "=r"(pml4), "=r"(pdpt_low), "=r"(pdpt_direct), "=r"(pdpt_kernel), "=r"(pd_kernel)
    );

    // 1. Low Identity Map (PML4[0]) - First 4GB with 1GB huge pages
    pml4[0] = reinterpret_cast<std::uint64_t>(pdpt_low) | 0x03; // Present, Writable
    for (std::uint64_t i = 0; i < 4; ++i) {
        pdpt_low[i] = (i * 0x40000000ULL) | 0x83; // Present, Writable, 1GB Huge
    }

    // 2. Direct Map (PML4[272] = 0xFFFF880000000000) - 64GB with 1GB huge pages
    pml4[272] = reinterpret_cast<std::uint64_t>(pdpt_direct) | 0x03; // Present, Writable
    for (std::uint64_t i = 0; i < 64; ++i) {
        pdpt_direct[i] = (i * 0x40000000ULL) | 0x83; // Present, Writable, 1GB Huge
    }

    // 3. Higher-Half Kernel Map (PML4[511] = 0xFFFFFFFF80000000) - 1GB with 2MB huge pages
    pml4[511] = reinterpret_cast<std::uint64_t>(pdpt_kernel) | 0x03;
    pdpt_kernel[510] = reinterpret_cast<std::uint64_t>(pd_kernel) | 0x03;
    for (std::uint64_t i = 0; i < 512; ++i) {
        pd_kernel[i] = (i * 0x200000ULL) | 0x83; // Present, Writable, 2MB Huge
    }

    // 4. Load CR3 with physical address of boot_pml4
    std::uint64_t pml4_phys = reinterpret_cast<std::uint64_t>(pml4);
    __asm__ volatile("movq %0, %%cr3" :: "r"(pml4_phys) : "memory");

    // 5. Switch to higher-half stack and jump to kernel_main
    std::uint8_t* stackBase = stack + sizeof(stack);
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

extern "C" void kernel_main(oz_boot::PlatformInfo* platformInfoPhys) {
    // 6. Convert PlatformInfo pointer to direct map virtual address
    oz_boot::PlatformInfo* platformInfo = oz::phys_to_virt(BootInfoProvider::getPhys(platformInfoPhys));

    // 7. Unmap lower identity mapping
    boot_pml4[0] = 0;
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
    setKernelPtr(static_cast<void*>(&k));
    new (static_cast<void*>(&k)) oz::Kernel{platformInfo};
    reinterpret_cast<oz::Kernel*>(&k)->run();

    while (1) __asm__ volatile("hlt");
}