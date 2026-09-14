// Shared counting allocator for the zero-allocation unit tests (G4 P1/P2:
// render_chain_planned and TruePeak::process must not allocate on the hot
// path). Replaces the global operator new/delete in THIS binary exactly once
// (a shared TU — both test translation units call into it); every heap
// allocation increments a program-wide counter read via sftest::alloc_calls().
#ifndef SF_TESTS_ALLOC_COUNTER_HPP
#define SF_TESTS_ALLOC_COUNTER_HPP

namespace sftest {

// Number of heap allocations since process start (monotone, relaxed atomics).
long alloc_calls();

}  // namespace sftest

#endif  // SF_TESTS_ALLOC_COUNTER_HPP