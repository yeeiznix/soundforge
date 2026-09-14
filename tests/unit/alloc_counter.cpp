// Shared counting allocator — see alloc_counter.hpp.
#include "alloc_counter.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
std::atomic<long> g_alloc_calls{0};

void* count_alloc(std::size_t n) {
  g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n);
  if (!p) throw std::bad_alloc();
  return p;
}
}  // namespace

void* operator new(std::size_t n) { return count_alloc(n); }
void* operator new[](std::size_t n) { return count_alloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace sftest {
long alloc_calls() { return g_alloc_calls.load(std::memory_order_relaxed); }
}  // namespace sftest