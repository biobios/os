#pragma once

namespace oz {
namespace x86_64 {
void initGDTR();
void setPageMap(void* map);
void enableSSE();
}
}  // namespace oz