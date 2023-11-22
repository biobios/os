#include "PageManager.hpp"

#include "x86_64.hpp"
#include "utils.hpp"
#include "Kernel.hpp"

oz::PageManager::PageManager() 
{
    pml4_table[0] = reinterpret_cast<std::uint64_t>(&pdp_table[0]) | 0x003;
    for(std::uint64_t i_pdpt = 0; i_pdpt < PAGE_DIRECTORY_COUNT; i_pdpt++){
        pdp_table[i_pdpt] = reinterpret_cast<std::uint64_t>(&page_directory[i_pdpt]) | 0x003;
        for(std::uint64_t i_pd = 0; i_pd < 512; i_pd++){
            page_directory[i_pdpt][i_pd] = (i_pdpt * (512*512*4096) + i_pd * (512*4096)) | 0x083;
        }
    }

    oz::x86_64::setPageMap(&pml4_table);
}