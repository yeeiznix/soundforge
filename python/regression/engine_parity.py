# SoundForge G6 — regression parity harness (docs/PLAN_G6.md §1.2, §3.4 D4).
"""Single source of truth for the audio parity tolerances + compare helpers.

Consumed by ``tests/python_tests/test_engine_parity.py`` (and by any future
P3 parity expansion). Tolerances are the ONLY numbers here — the tests never
hard-code their own (ORC-G6-10):

- meter ``|native − reference| ≤ 1e-3`` (DC only — mirrors
  ``test_audio_engine.cpp`` meter tolerance; D5: the reference meter is the
  exact true peak of DC, no FIR);
- DC-anchor block samples ``≤ 1e-6`` **absolute** (mirrors
  ``test_audio_engine.cpp:941-942``);
- sine block samples ``≤ 1e-5`` **relative** (``1e-5 × (1 + |ref|)`` —
  float64 reference vs float32 engine).

Signal generators (:func:`dc_source`, :func:`sine_source`) and
:class:`BlockCapture` are the *same* Python signal source the ctypes io
callbacks and the reference engine both use — divergence can then only come
from the engine's own processing (D4 rationale).

Callback contract (SEC-G6-05): every callback asserts ``channels == 2`` and
``frames >= 1`` at the top (tripwire → record + ``SF_E_IO``), wraps its body
in ``try/except`` (exception → record + ``SF_E_IO``; never let it escape —
ctypes would print and return 0 == SF_OK, treating half-filled input as
valid), and copies data **within the call** — pointers are never retained or
dereferenced after return. The ``CFUNCTYPE`` instances (``.cb``) are kept
alive as attributes of the :class:`~soundforge_py.engine.AudioEngine` wrapper
for the engine lifetime (the native side stores raw function pointers).
"""

from __future__ import annotations

import ctypes
import math
from ctypes import POINTER, c_float, cast

from soundforge_py.engine import ReadIoFn, SF_E_IO, SF_OK, WriteIoFn

# --- Tolerances (single source of truth — ORC-G6-10) ------------------------

# Meter parity: |native − reference| ≤ 1e-3 (DC only; mirrors the G5 unit
# tests' meter tolerance).
TOL_METER_DC = 1e-3

# DC-anchor block parity: absolute ≤ 1e-6 (mirrors test_audio_engine.cpp:941-942).
TOL_DC_BLOCK_ABS = 1e-6

# Sine block parity: relative ≤ 1e-5, i.e. 1e-5 × (1 + |ref|) per sample
# (float64 reference vs float32 engine).
TOL_SINE_BLOCK_REL = 1e-5

# --- Compare helpers --------------------------------------------------------


def compare_meter(native: float, reference: float, kind: str = "dc") -> None:
    """Assert ``|native − reference| ≤ 1e-3``.

    DC-only by contract (D5/G6-1): the reference meter is the exact true
    peak of DC; sine is used for output-**block** parity only.
    """
    if kind != "dc":
        raise AssertionError(
            f"meter parity is DC-only (D5/G6-1); got kind={kind!r}"
        )
    delta = abs(native - reference)
    assert delta <= TOL_METER_DC, (
        f"meter parity: |native−reference|={delta:.3e} > {TOL_METER_DC} "
        f"(native={native!r}, reference={reference!r})"
    )


def compare_blocks(
    native: list[float], reference: list[float], kind: str = "dc"
) -> None:
    """Assert block-sample parity: DC absolute ≤ 1e-6, sine relative ≤ 1e-5.

    ``native`` = captured float32 samples (converted to Python floats);
    ``reference`` = the float64 pure-Python render.
    """
    assert len(native) == len(reference), (
        f"block length mismatch: native={len(native)} reference={len(reference)}"
    )
    if kind == "dc":
        for i, (n, r) in enumerate(zip(native, reference)):
            delta = abs(n - r)
            assert delta <= TOL_DC_BLOCK_ABS, (
                f"dc block sample[{i}]: |native−reference|={delta:.3e} "
                f"> {TOL_DC_BLOCK_ABS} (native={n!r}, reference={r!r})"
            )
    elif kind == "sine":
        for i, (n, r) in enumerate(zip(native, reference)):
            tol = TOL_SINE_BLOCK_REL * (1.0 + abs(r))
            delta = abs(n - r)
            assert delta <= tol, (
                f"sine block sample[{i}]: |native−reference|={delta:.3e} "
                f"> {tol:.3e} (native={n!r}, reference={r!r})"
            )
    else:
        raise AssertionError(f"unknown block compare kind {kind!r}")


# --- Signal generators (shared by native callbacks and the reference) ------


def _read_cb(name: str, fill, errors: list[str]) -> ReadIoFn:
    """Wrap a buffer-filling callable in a CFUNCTYPE read callback.

    Tripwires (``channels == 2``, ``frames >= 1``) run first; the body is
    wrapped in ``try/except``; every failure records into the per-session
    ``errors`` list and returns ``SF_E_IO`` (SEC-G6-05).
    """

    def _impl(user, in_ptrs, channels, frames) -> int:
        try:
            if channels != 2:
                errors.append(f"{name}.read: channels=={channels} != 2")
                return SF_E_IO
            if frames < 1:
                errors.append(f"{name}.read: frames=={frames} < 1")
                return SF_E_IO
            fill(in_ptrs, channels, frames)
            return SF_OK
        except Exception as exc:  # noqa: BLE001 — must never escape (SEC-G6-05)
            errors.append(f"{name}.read exception: {exc!r}")
            return SF_E_IO

    return ReadIoFn(_impl)


def _write_cb(name: str, on_block, errors: list[str]) -> WriteIoFn:
    """Wrap a block-consuming callable in a CFUNCTYPE write callback.

    Same tripwires + ``try/except`` contract as :func:`_read_cb`; the return
    value is intentionally not propagated by the engine (block already
    rendered), but we still return ``SF_E_IO`` on failure (SEC-G6-05).
    """

    def _impl(user, out_ptrs, channels, frames) -> int:
        try:
            if channels != 2:
                errors.append(f"{name}.write: channels=={channels} != 2")
                return SF_E_IO
            if frames < 1:
                errors.append(f"{name}.write: frames=={frames} < 1")
                return SF_E_IO
            on_block(out_ptrs, channels, frames)
            return SF_OK
        except Exception as exc:  # noqa: BLE001 — must never escape (SEC-G6-05)
            errors.append(f"{name}.write exception: {exc!r}")
            return SF_E_IO

    return WriteIoFn(_impl)


class DcSource:
    """Constant (DC) signal source — shared by native read callback and
    reference. ``.cb`` is the CFUNCTYPE instance; ``.block(frames)`` produces
    the identical float64 block the reference renders; ``.errors`` is the
    per-session callback error list."""

    def __init__(self, dc: float) -> None:
        self.dc = float(dc)
        self.errors: list[str] = []
        self.cb = _read_cb("dc_source", self._fill, self.errors)

    def _fill(self, in_ptrs, channels: int, frames: int) -> None:
        dc = c_float(self.dc)
        for c in range(channels):
            arr = cast(in_ptrs[c], POINTER(c_float * frames)).contents
            for i in range(frames):
                arr[i] = dc

    def block(self, frames: int) -> list[float]:
        return [self.dc] * frames


class SineSource:
    """Fixed-frequency sine source — shared by native read callback and
    reference. Both channels carry the same samples; the phase advances by
    ``2π·freq/sample_rate`` per sample in BOTH ``_fill`` and ``block``, so the
    native tick and the reference render consume the identical signal
    (deterministic; no wall clock)."""

    def __init__(self, freq: float, sample_rate: float = 48000.0) -> None:
        self.freq = float(freq)
        self.sample_rate = float(sample_rate)
        self.phase = 0.0
        self.errors: list[str] = []
        self.cb = _read_cb("sine_source", self._fill, self.errors)

    def _incr(self) -> float:
        return 2.0 * math.pi * self.freq / self.sample_rate

    def _fill(self, in_ptrs, channels: int, frames: int) -> None:
        incr = self._incr()
        for c in range(channels):
            arr = cast(in_ptrs[c], POINTER(c_float * frames)).contents
            phase = self.phase
            for i in range(frames):
                arr[i] = c_float(math.sin(phase))
                phase += incr
        self.phase = math.fmod(self.phase + incr * frames, 2.0 * math.pi)

    def block(self, frames: int) -> list[float]:
        incr = self._incr()
        out = [math.sin(self.phase + incr * i) for i in range(frames)]
        self.phase = math.fmod(self.phase + incr * frames, 2.0 * math.pi)
        return out


class BlockCapture:
    """Write callback that copies each rendered output block out **within the
    call** (never retaining pointers): ``.blocks`` grows one ``(left, right)``
    pair of Python-float lists per rendered block."""

    def __init__(self) -> None:
        self.blocks: list[tuple[list[float], list[float]]] = []
        self.errors: list[str] = []
        self.cb = _write_cb("block_capture", self._on_block, self.errors)

    def _on_block(self, out_ptrs, channels: int, frames: int) -> None:
        arr_l = cast(out_ptrs[0], POINTER(c_float * frames)).contents
        arr_r = cast(out_ptrs[1], POINTER(c_float * frames)).contents
        left = [float(arr_l[i]) for i in range(frames)]
        right = [float(arr_r[i]) for i in range(frames)]
        self.blocks.append((left, right))


# Public aliases used by the parity tests / reference layer.
dc_source = DcSource
sine_source = SineSource


__all__ = [
    "TOL_METER_DC",
    "TOL_DC_BLOCK_ABS",
    "TOL_SINE_BLOCK_REL",
    "compare_meter",
    "compare_blocks",
    "DcSource",
    "SineSource",
    "BlockCapture",
    "dc_source",
    "sine_source",
]