/*
 * SPDX-License-Identifier: MIT
 *
 * Counting allocator for the monitor's "no allocation on the tap path" tests: replaces the global operator new/delete of this
 * test binary and counts every allocation.
 */
#include <atomic>
#include <cstdlib>
#include <new>

static std::atomic<long long> g_allocs{0};

long long alloc_count() { return g_allocs.load(); }

void *operator new(std::size_t n) {
    ++g_allocs;
    if (void *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) {
    ++g_allocs;
    if (void *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
