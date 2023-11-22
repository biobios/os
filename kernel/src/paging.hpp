#pragma once
#include <cstddef>

namespace oz{
    namespace paging{
        constexpr std::size_t uefi_page_size = 4 * 1024;

        namespace x86_64 {
            constexpr std::size_t page_sizes[] = {4 * 1024, 2 * 1024 * 1024, 1024 * 1024 * 1024};
        }
    }
}