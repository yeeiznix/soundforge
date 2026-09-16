"""Pure-Python reference engine for SoundForge G6 (docs/PLAN_G6.md §3.5 D5).

Minimal-but-honest chain renderer: deterministic parity oracle for the native
C++ engine on chain-gain surfaces only (no NumPy, no pan/solo/multi-source).
Used by the pytest parity suite to verify tick/meter/block lockstep.

Authority: the C++ engine (``libsoundforge.so`` via ctypes) is authoritative.
The reference computes the **same deterministic surfaces** and serves as the
expectation.
"""
