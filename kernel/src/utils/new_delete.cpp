#include "utils/new_delete.hpp"
#include <cstddef>
#include <new>

static void* (*g_malloc)(std::size_t) = nullptr;
static void (*g_free)(void*) = nullptr;

namespace oz {
    void setMallocAndFree(void* (*malloc_func)(std::size_t), void (*free_func)(void*)) {
        g_malloc = malloc_func;
        g_free = free_func;
    }
}

void* operator new(std::size_t size) {
    if (g_malloc) return g_malloc(size);
    return nullptr;
}
void* operator new[](std::size_t size) {
    if (g_malloc) return g_malloc(size);
    return nullptr;
}
void operator delete(void* p) noexcept {
    if (g_free) g_free(p);
}
void operator delete[](void* p) noexcept {
    if (g_free) g_free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    if (g_free) g_free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    if (g_free) g_free(p);
}
