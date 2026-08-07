#pragma once
#include "PageTableManager.hpp"

namespace oz {
    template <frame_manager FrameManager>
    using PageManager = PageTableManager<FrameManager>;
}