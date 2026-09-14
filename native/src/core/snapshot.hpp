// SoundForge G4 P3 — PlanSnapshotStore: single-writer/single-reader 4-slot
// lock-free RenderPlan handoff (PLAN_G4 §4.1 D1, review R-A amendment).
//
// Writer = the queue-runner thread at publish time (it is the mutation owner,
// so it may read proj->doc.signalGraph); reader = the engine tick/pacer thread
// (which must NEVER touch proj->doc). Internal C++ only — no public header,
// no export, no JNI, no schema/ABI change.
//
// Protocol (exact D1):
//   slot state ∈ { EMPTY, WRITING, READY, READING }, atomic per slot; each slot
//   owns one RenderPlan BY VALUE plus an atomic<uint64_t> seq.
//
//   PUBLISH (writer, wait-free): scan for an EMPTY slot -> CAS(EMPTY,WRITING);
//   otherwise pick the READY slot with the SMALLEST seq -> CAS(READY,WRITING);
//   compile the plan INTO the slot; SUCCESS: slot.seq = ++global_seq,
//   state.store(READY, release); FAILURE: state.store(EMPTY, release) — a
//   torn/null plan is never published and the previous READY plan stays
//   current.
//
//   ACQUIRE (reader, lock-free, ≤2 slots transiently held): find the READY slot
//   with the LARGEST seq -> CAS(READY,READING); on success release the
//   PREVIOUSLY-held slot (READING -> EMPTY); no READY keeps the plan already
//   held; the first-ever tick with nothing published returns nullptr (silence
//   allowed).
//
// Why 4 slots (R-A): at any instant the writer holds ≤1 slot WRITING and the
// reader holds ≤1 READING at rest / ≤2 transiently (it claims the new slot
// before releasing the old), so ≤3 slots are non-claimable and at least one of
// the 4 is always EMPTY/READY for the writer — publish never spins. The
// READY-reclaim CAS can lose to the reader claiming the same slot (the scan
// retries); cas_failures() exposes that counter so a test can assert it stays
// 0 under the fast-path schedule. The EMPTY-path CAS loss is unreachable by
// construction — only the writer ever sets WRITING from EMPTY — and is counted
// purely defensively.
//
// Storage decision: each slot stores its RenderPlan BY VALUE (not shared_ptr).
// compile_render_plan compiles "INTO slot.plan" (D1 step 2) with no extra
// allocation for the plan object itself, and the single-reader engine mutates
// the plan's owned per-node AudioBlock scratch during execute — which is safe
// precisely because exactly one reader holds a slot in READING at a time. A
// shared/refcounted plan would add an allocation + atomics to the publish path
// and buy nothing. Memory is bounded at 4 plans (N × ~4 KB per graph node).
//
// Exception containment (ORC-G4-03): compile_render_plan calls topological_order
// OUTSIDE its own try/catch (render_plan.cpp:25), so a publish-path compile can
// still throw (e.g. std::bad_alloc from the topology sets/vectors). publish()
// wraps the WHOLE compile — including topological_order — in catch(...): the
// slot is restored to EMPTY, the last valid plan stays current, no WRITING slot
// leaks, and no exception escapes the runner thread. bad_alloc is mapped to an
// out-of-memory err string (P4 maps that to SF_E_NOMEM).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "render_plan.hpp"

namespace sfcore {

struct SignalGraphDoc;  // graph_internal.hpp

class PlanSnapshotStore {
 public:
  static constexpr int kSlotCount = 4;

  enum SlotState : int {
    kSlotEmpty = 0,
    kSlotWriting = 1,
    kSlotReady = 2,
    kSlotReading = 3,
  };

  // Fault-injection seam used ONLY by tests: the default (nullptr) is the real
  // dsp::compile_render_plan. A throwing hook lets the compile-failure tests
  // exercise the publish-path containment (bad_alloc / arbitrary exception)
  // without needing a genuine allocation failure.
  using CompileFn = bool (*)(const SignalGraphDoc&, const std::string&,
                             dsp::RenderPlan&, std::string&);

  PlanSnapshotStore() = default;
  PlanSnapshotStore(const PlanSnapshotStore&) = delete;
  PlanSnapshotStore& operator=(const PlanSnapshotStore&) = delete;

  // Writer-side (runner thread). Returns true when a fresh plan was published
  // (slot READY, seq assigned); false on compile failure, in which case the
  // slot is restored to EMPTY and *err holds the reason. Never throws.
  bool publish(const SignalGraphDoc& graph, const std::string& out_node_id,
               std::string& err, CompileFn compile = nullptr);

  // Reader-side (tick/pacer thread). Returns the plan to render this tick: the
  // newly-acquired newest READY plan, else the previously-held plan, else
  // nullptr on the first-ever tick with nothing published. Non-const because
  // the single reader owns the slot exclusively while READING and mutates the
  // plan's per-node AudioBlock scratch during execute (render_plan.hpp:19-20).
  // *seq_out (optional) receives the held plan's seq (0 when nothing is held).
  dsp::RenderPlan* acquire(uint64_t* seq_out = nullptr);

  // Releases the reader's currently-held slot back to EMPTY (teardown).
  void release();

  // Introspection (tests / diagnostics). Thread-safe loads only.
  int slot_state(int i) const;    // SlotState as int, -1 if out of range
  uint64_t slot_seq(int i) const; // 0 if out of range / never published
  // Reader-only: index of the slot currently held READING (-1 = none). Not
  // safe for cross-thread reads — it is plain state owned by the reader
  // (writer-side introspection must use slot_state/slot_seq).
  int held_slot() const { return held_; }
  uint64_t published_seq() const {
    return published_seq_.load(std::memory_order_relaxed);
  }
  // Number of CAS losses where the candidate slot was claimed by the other
  // side between scan and CAS. The scan simply retries — this is lock-free
  // progress, NOT a spin on the reader (see spin_count()).
  uint64_t cas_failures() const {
    return cas_failures_.load(std::memory_order_relaxed);
  }
  // Number of full scan passes that found NO claimable slot at all (no EMPTY,
  // no READY) — the only state in which the writer would have to wait for the
  // reader. The 4-slot proof (≤1 WRITING + ≤2 transient READING < 4) makes this
  // unreachable, so it stays 0; the stress test asserts exactly that.
  uint64_t spin_count() const {
    return spin_count_.load(std::memory_order_relaxed);
  }

 private:
  struct Slot {
    std::atomic<int> state{static_cast<int>(kSlotEmpty)};
    std::atomic<uint64_t> seq{0};
    dsp::RenderPlan plan;
  };

  // Claims a slot (EMPTY preferred, else oldest READY) and returns its index.
  // Bounded retry; returns -1 only on an impossible exhaustion (defensive).
  int claim_slot();

  Slot slots_[kSlotCount];
  uint64_t next_seq_ = 0;  // writer-only; the published seq is mirrored below
  std::atomic<uint64_t> published_seq_{0};
  std::atomic<uint64_t> cas_failures_{0};
  std::atomic<uint64_t> spin_count_{0};
  int held_ = -1;          // reader-only: index of the READING slot
  uint64_t held_seq_ = 0;  // reader-only
};

}  // namespace sfcore
