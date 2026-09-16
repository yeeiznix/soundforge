# SoundForge G6 — D5 pure-Python reference engine (docs/PLAN_G6.md §3.5).
"""Minimal-but-honest pure-Python chain renderer used as the parity expectation.

The C++ engine under ``native/`` is authoritative; this module computes the
**chain-gain surface only** (PLAN_G6.md §3.5 D5):

- signal-graph topo order (asserts chain shape — non-chain graphs raise
  ``ValueError`` with a fixture-driven reason);
- per-node linear gain ``10^(gainDb/20)`` from ``node.mixer.gainDb`` (default
  ``0.0`` when absent; ``mute`` → 0.0), applied in topo order
  (``render_plan.cpp:161-176`` semantics: every node block, including the
  source's first block, is gained; output = target node's block);
- input block generation via the **same** Python signal source the ctypes read
  callback uses (``dc_source(dc)`` / ``sine_source(freq)`` from
  ``regression/engine_parity.py``);
- reference meter: max abs over the rendered output block's L/R — exact for
  DC (true peak of DC == amplitude exactly — no FIR needed).

Deliberately **out of scope** (D5 boundary): pan/mixer routing, multi-source
graphs, solo logic on the reference side, TruePeak 4× oversample FIR, any
meter handling for sine (G6-1: sine is used for output-**block** parity only).
The single source driver is the same object the native read callback uses, so
divergence can only come from the engine's own processing.

This module is stdlib-only (``math``) — the G6 layer adds no dependency.
"""

from __future__ import annotations

import math
from typing import Any, Callable

# A Python signal source: ``block(frames) -> list[float]`` producing the next
# input block (the SAME object the ctypes read callback uses).
SignalSource = Callable[[int], list[float]]


def _linear_gain(node: dict[str, Any]) -> float:
    """Per-node linear gain: ``10^(gainDb/20)``; ``mute`` → 0.0.

    Mirrors ``render_plan.cpp``: ``pn.gain = pow(10.0, nd->mixer.gainDb / 20.0)``
    and the muted-exclusion (excluded nodes render zeros). ``gainDb`` defaults
    to ``0.0`` when absent (schema default).
    """
    mixer = node.get("mixer") or {}
    if mixer.get("mute"):
        return 0.0
    gain_db = mixer.get("gainDb", 0.0)
    return math.pow(10.0, gain_db / 20.0)


def _chain_order(graph: dict[str, Any], why: str) -> list[str]:
    """Topo order of a **chain** (each node ≤1 in / ≤1 out edge, one source).

    Raises ``ValueError`` (with the D5 fixture-driven reason) on non-chain
    shapes: duplicate ids, dangling edges, fan-in/fan-out > 1, multiple
    sources, disconnected/cyclic structure. Cycles are already rejected by
    validate — but the reference refuses with its own message anyway.
    """
    nodes = graph.get("nodes") or []
    edges = graph.get("edges") or []
    ids = [n.get("id") for n in graph["nodes"] if isinstance(n, dict)]
    if len(ids) != len(set(ids)):
        raise ValueError(f"{why}: non-chain graph: duplicate node ids")
    in_deg: dict[str, int] = {i: 0 for i in ids}
    out_deg: dict[str, int] = {i: 0 for i in ids}
    adj: dict[str, list[str]] = {i: [] for i in ids}
    for e in graph["edges"] if isinstance(graph["edges"], list) else []:
        frm = e.get("from")
        to = e.get("to")
        if frm not in in_deg or to not in in_deg:
            raise ValueError(f"{why}: dangling edge {frm!r} -> {to!r}")
        in_deg[to] += 1
        out_deg[frm] += 1
        adj[frm].append(to)
    if any(d > 1 for d in in_deg.values()):
        raise ValueError(f"{why}: non-chain graph: fan-in")
    if any(d > 1 for d in out_deg.values()):
        raise ValueError(f"{why}: non-chain graph: fan-out > 1 (multi-output)")
    sources = [i for i in ids if in_deg[i] == 0]
    if len(sources) != 1:
        raise ValueError(
            f"{why}: need exactly one source node, got {len(sources)}")
    order = [sources[0]]
    while adj.get(order[-1]):
        nxts = adj[order[-1]]
        if len(nxts) != 1:  # fan-out > 1 — cannot happen (checked)
            raise ValueError(f"{why}: non-chain graph: fan-out > 1")
        order.append(nxts[0])
    if set(order) != set(ids):
        raise ValueError(f"{why}: non-chain graph: disconnected/cyclic nodes")
    return order


def render_chain(
    doc: dict[str, Any],
    out_node_id: str,
    source: SignalSource,
    frames: int,
    why: str = "reference: non-chain graph",
) -> list[float]:
    """Render one ``frames``-sample block through a fixed-gain chain.

    ``source`` is the shared Python signal source (``block(frames)``); every
    node's linear gain is applied in chain topo order; the returned block is
    the target node's block (L and R use the same samples — the ctypes read
    fills both channels from the same source).
    """
    graph = doc.get("signalGraph")
    if not isinstance(graph, dict):
        raise ValueError(f"{why}: no signalGraph document")
    order = _chain_order(graph, why)
    if out_node_id not in order:
        raise ValueError(f"{why}: output node {out_node_id!r} not in chain")
    node_map = {n["id"]: n for n in graph["nodes"]}
    gains = [(_linear_gain(node_map[nid]), nid) for nid in order]
    blk = source.block(frames)
    for gain, _ in gains:
        if gain == 0.0:
            blk = [0.0] * frames
        elif gain != 1.0:
            blk = [x * gain for x in blk]
    return blk


class ReferenceEngine:
    """One-shot pure-Python chain renderer + block/meter oracle.

    ``blocks_rendered`` counts ``render`` calls — the lockstep counterpart of
    the native ``meter_json["blocksRendered"]`` (reset_meters does NOT reset it
    — monotonic on both sides).
    """

    def __init__(self) -> None:
        self.blocks_rendered = 0

    def render(
        self,
        doc: dict[str, Any],
        out_node_id: str,
        source: SignalSource,
        frames: int,
    ) -> tuple[list[float], tuple[float, float]]:
        """Render one block; returns ``(block, (meter_l, meter_r))``.

        The meter is the true-peak over the output block (for DC == amplitude
        exactly — no FIR/oversampling on the reference side; sine is used for
        output-block parity only, D5/G6-1).
        """
        blk = render_chain(doc, out_node_id, source, frames)
        m = max((abs(x) for x in blk), default=0.0)
        self.blocks_rendered += 1
        return blk, (m, m)

    def meter(self, l: list[float], r: list[float]) -> tuple[float, float]:
        """Reference meter over an already-rendered block's L/R."""
        ml = max((abs(x) for x in l), default=0.0)
        mr = max((abs(x) for x in r), default=0.0)
        return ml, mr