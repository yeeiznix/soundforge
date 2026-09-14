// SoundForge G4 P3 — PlanSnapshotStore implementation (PLAN_G4 §4.1 D1).
// See snapshot.hpp for the protocol, the 4-slot wait-free proof and the
// storage/exceptions rationale.
#include "snapshot.hpp"

#include <new>

namespace sfcore {

namespace {

// Cheap, static error texts for the publish failure path (ORC-G4P3-01). The
// exception handlers must NEVER allocate: a std::string assignment/concat in a
// handler can itself throw bad_alloc under genuine OOM, and an exception
// escaping the handler would skip the slot restore and strand it WRITING
// forever (4 such leaks exhaust the store; "publish() never throws" —
// snapshot.hpp:85). These literals have static storage; assignment to `err` is
// deferred until AFTER the slot has already been restored to EMPTY and is
// fenced, so it is best-effort only.
constexpr const char* kErrCompileOom = "render_plan: out of memory";
constexpr const char* kErrCompileException = "render_plan: compile exception";
constexpr const char* kErrCompileUnknown = "render_plan: unknown compile exception";
constexpr const char* kErrCompileFailed = "render_plan: compile failed";
constexpr const char* kErrNoSlot = "render_plan: snapshot store has no claimable slot";

}  // namespace

int PlanSnapshotStore::slot_state(int i) const {
  if (i < 0 || i >= kSlotCount) return -1;
  return slots_[i].state.load(std::memory_order_acquire);
}

uint64_t PlanSnapshotStore::slot_seq(int i) const {
  if (i < 0 || i >= kSlotCount) return 0;
  return slots_[i].seq.load(std::memory_order_acquire);
}

int PlanSnapshotStore::claim_slot() {
  // Each pass re-scans from scratch: a lost CAS (the reader just claimed the
  // candidate) means the slot inventory changed, so the next pass may find a
  // different EMPTY/READY slot. The pass bound is purely defensive — the
  // 4-slot argument below makes -1 unreachable.
  for (int pass = 0; pass < 1024; ++pass) {
    // (1) Prefer any EMPTY slot. Load-then-CAS: a CAS loss is only counted when
    // the slot really was EMPTY at the scan load (i.e. the other side claimed
    // it in between); a slot seen non-EMPTY is simply skipped. Note this
    // EMPTY-path loss is unreachable by construction — only the writer ever
    // sets WRITING from EMPTY (the reader only CASes READY->READING and stores
    // READING->EMPTY on release) and there is exactly one writer — so the
    // counter below is purely defensive.
    for (int i = 0; i < kSlotCount; ++i) {
      if (slots_[i].state.load(std::memory_order_acquire) != kSlotEmpty) continue;
      int expected = kSlotEmpty;
      if (slots_[i].state.compare_exchange_strong(
              expected, kSlotWriting, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        return i;
      }
      cas_failures_.fetch_add(1, std::memory_order_relaxed);
    }

    // (2) No EMPTY: reclaim the READY slot with the SMALLEST seq (the oldest —
    // the reader targets the newest, so a CAS resolves the contention).
    int oldest = -1;
    uint64_t oldest_seq = 0;
    for (int i = 0; i < kSlotCount; ++i) {
      if (slots_[i].state.load(std::memory_order_acquire) != kSlotReady) continue;
      const uint64_t s = slots_[i].seq.load(std::memory_order_relaxed);
      if (oldest < 0 || s < oldest_seq) {
        oldest = i;
        oldest_seq = s;
      }
    }
    if (oldest >= 0) {
      int expected = kSlotReady;
      if (slots_[oldest].state.compare_exchange_strong(
              expected, kSlotWriting, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        return oldest;
      }
      // This READY->WRITING CAS loss IS reachable: the reader can claim the
      // candidate (READY->READING) between the scan above and this CAS. Unlike
      // the EMPTY-path counter (defensive only), this is the contention the
      // scan-retry design targets.
      cas_failures_.fetch_add(1, std::memory_order_relaxed);
      continue;  // inventory changed; rescan
    }

    // No EMPTY and no READY: the writer would have to wait for the reader.
    // Wait-free proof: writer holds <=1 WRITING, reader holds <=1 READING at
    // rest / <=2 transiently (it claims before releasing) => <=3 of 4 slots are
    // ever non-claimable, so this branch is unreachable. Counted so the stress
    // test can assert 0.
    spin_count_.fetch_add(1, std::memory_order_relaxed);
  }
  return -1;  // defensive only
}

bool PlanSnapshotStore::publish(const SignalGraphDoc& graph,
                                const std::string& out_node_id, std::string& err,
                                CompileFn compile) {
  const int idx = claim_slot();
  if (idx < 0) {
    err = kErrNoSlot;
    return false;
  }
  Slot& slot = slots_[idx];

  err.clear();
  bool ok = false;
  // ORC-G4-03: compile_render_plan calls topological_order OUTSIDE its own try
  // (render_plan.cpp:25), so the WHOLE compile — topological_order included —
  // must be contained here. A throwing hook (tests) is contained identically.
  //
  // ORC-G4P3-01: the handlers below NEVER allocate — no string building, no
  // concatenation of e.what(). Under genuine OOM a std::string assignment in a
  // handler can itself throw bad_alloc; an exception escaping the handler
  // would skip the slot restore and leak it in WRITING forever (4 such leaks
  // exhaust the store and break "publish() never throws"). The failure kind is
  // recorded here as a pointer to a static literal and formatted only AFTER
  // the slot is restored, at the top of the !ok block below.
  const char* fail_reason = nullptr;  // static literal; never allocates
  try {
    ok = compile ? compile(graph, out_node_id, slot.plan, err)
                 : dsp::compile_render_plan(graph, out_node_id, slot.plan, err);
  } catch (const std::bad_alloc&) {
    ok = false;
    fail_reason = kErrCompileOom;
  } catch (const std::exception&) {
    ok = false;
    fail_reason = kErrCompileException;
  } catch (...) {
    ok = false;
    fail_reason = kErrCompileUnknown;
  }

  if (!ok) {
    // Never publish a torn/null plan and never leak WRITING: restore EMPTY
    // FIRST — before ANY error-text work. Move-assigning an empty plan is
    // noexcept (std::allocator) and frees the dead slot's pool; the store
    // makes the EMPTY state visible with a release so the writer's next claim
    // can reuse the slot. From here on nothing can throw out of publish().
    slot.plan = dsp::RenderPlan{};
    slot.state.store(kSlotEmpty, std::memory_order_release);
    // Best-effort message, formatted AFTER the slot is safe (a compile that
    // returned false, e.g. a cycle, has already written its own err, which is
    // kept). Only static literals are used, and even those are assigned under
    // a guard so a residual bad_alloc can neither escape publish() nor corrupt
    // the store — err may stay empty/partial, which is fine: the failure
    // verdict and the slot state are already decided.
    if (fail_reason != nullptr) {
      try {
        err = fail_reason;
      } catch (...) {
      }
    } else if (err.empty()) {
      try {
        err = kErrCompileFailed;
      } catch (...) {
      }
    }
    return false;
  }

  // Success: assign the seq then publish with a release store so a reader that
  // acquires the slot (CAS READY->READING) observes the compiled plan.
  const uint64_t seq = ++next_seq_;
  slot.seq.store(seq, std::memory_order_relaxed);
  published_seq_.store(seq, std::memory_order_release);
  slot.state.store(kSlotReady, std::memory_order_release);
  return true;
}

dsp::RenderPlan* PlanSnapshotStore::acquire(uint64_t* seq_out) {
  // (1) READY slot with the LARGEST seq (the newest plan).
  int best = -1;
  uint64_t best_seq = 0;
  for (int i = 0; i < kSlotCount; ++i) {
    if (slots_[i].state.load(std::memory_order_acquire) != kSlotReady) continue;
    const uint64_t s = slots_[i].seq.load(std::memory_order_relaxed);
    if (best < 0 || s > best_seq) {
      best = i;
      best_seq = s;
    }
  }

  // Monotone refinement of D1 step (1) demanded by "the reader acquires the
  // newest plan and KEEPS it across ticks": once a plan is held, only a
  // strictly-newer READY plan may replace it. Older READY leftovers are left
  // for the writer's smallest-seq reclaim (D1: "writer targets the oldest
  // READY") — the reader must never render a plan older than the one it holds.
  if (best >= 0 && (held_ < 0 || best_seq > held_seq_)) {
    int expected = kSlotReady;
    if (slots_[best].state.compare_exchange_strong(
            expected, kSlotReading, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      // Re-read the AUTHORITATIVE seq once the slot is ours (READING): the
      // scanned best_seq can be stale. The writer reclaims the SMALLEST-seq
      // READY slot — if `best` was the only READY slot it is also the smallest,
      // so the writer may have reclaimed it, recompiled and republished it
      // (READY->WRITING->READY with a fresh seq) between our seq load and the
      // CAS. The CAS then succeeds on the *republished* slot, so the seq we
      // scanned no longer describes its plan. While READING the writer cannot
      // touch the slot, so this reload is stable; it is always >= best_seq
      // (the writer only ever assigns increasing seqs) and therefore still
      // strictly newer than any held plan, preserving the monotone guard.
      const uint64_t actual_seq =
          slots_[best].seq.load(std::memory_order_acquire);
      // (2) Release the PREVIOUSLY-held slot only after the new one is claimed
      // (the <=2 transient READING window D1 relies on).
      if (held_ >= 0 && held_ != best) {
        slots_[held_].state.store(kSlotEmpty, std::memory_order_release);
      }
      held_ = best;
      held_seq_ = actual_seq;
    } else {
      // The writer reclaimed the candidate (READY->WRITING) between scan and
      // CAS; keep whatever we already hold — the newer plan is picked up next
      // tick (eventual, never lost).
      cas_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  if (held_ >= 0) {
    if (seq_out) *seq_out = held_seq_;
    return &slots_[held_].plan;
  }
  // No READY and nothing held: the first-ever tick renders silence.
  if (seq_out) *seq_out = 0;
  return nullptr;
}

void PlanSnapshotStore::release() {
  if (held_ < 0) return;
  slots_[held_].state.store(kSlotEmpty, std::memory_order_release);
  held_ = -1;
  held_seq_ = 0;
}

}  // namespace sfcore
