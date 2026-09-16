"""Regression parity harness for SoundForge G6 (docs/PLAN_G6.md §1.2, §3.4 D4).

Single source of truth for audio parity tolerances, signal generators, and
compare helpers (ORC-G6-10). Consumed by the pytest parity suite to verify
deterministic lockstep on meter/block surfaces.

Callback contract (SEC-G6-05): io callbacks copy within the call, wrap in
try/except → SF_E_IO on failure, assert channels==2 tripwire.
"""
