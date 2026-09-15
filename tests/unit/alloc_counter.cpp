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

// G4-9: over-aligned allocations bypass the default-aligned operator new, so
// the zero-alloc hot-path assertions could be evaded by an over-aligned type.
// Count them through the same counter (std::aligned_alloc carries the
// alignment). Mirrors the default-aligned counting functions exactly.
void* count_alloc_aligned(std::size_t n, std::align_val_t align) {
  const std::size_t a = static_cast<std::size_t>(align);
  if (a < alignof(std::max_align_t) || (a & (a - 1)) != 0) {
    // std::aligned_alloc requires a power of two >= the fundamental alignment;
    // fall back to the default-aligned path for degenerate alignments.
    return count_alloc(n);
  }
  g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
  // C11 requires size to be an integral multiple of alignment (and nonzero).
  const std::size_t rounded = n == 0 ? a : ((n + a - 1) / a) * a;
  void* p = std::aligned_alloc(a, rounded);
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

// G4-9 aligned overloads — counted through the same counter so the zero-alloc
// delta assertions hold against over-aligned types too.
void* operator new(std::size_t n, std::align_val_t align) {
  return count_alloc_aligned(n, align);
}
void* operator new[](std::size_t n, std::align_val_t align) {
  return count_alloc_aligned(n, align);
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}

namespace sftest {
long alloc_calls() { return g_alloc_calls.load(std::memory_order_relaxed); }
}  // namespace sftest
