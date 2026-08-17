#pragma once
#include "memory/PageTableManager.hpp"

namespace oz {
    template <frame_manager FrameManager>
    using PageManager = PageTableManager<FrameManager>;
}