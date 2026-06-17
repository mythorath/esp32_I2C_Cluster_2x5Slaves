#!/usr/bin/env python3
"""Validate the distributed halo-exchange ring (Phase 4-6) against a monolith.

Mirrors the master + N nodes: N strips each compute Lenia independently and
exchange uint8-quantized boundary rows around the torus using the master's
routing rule:

    strip i  TOP halo  <- strip (i-1) BOTTOM edge
    strip i  BOTTOM halo <- strip (i+1) TOP edge   (mod N)

We compare against a single monolithic W x (N*IH) torus and confirm:
  (a) the distributed ring stays alive and tracks the monolith's mass, and
  (b) a glider crosses strip boundaries (its global COM sweeps the full height),
which is the Phase-4 "validate against the desktop two-strip output" gate, and
the same logic that carries through to the two A<->B seams in Phase 6.

Run: python ring_check.py
"""
from __future__ import annotations

import numpy as np

from lenia_ref import LeniaParams, World, ORBIUM, bell
from port_check import build_taps, growth


def run_monolith(p, N, IH, W, steps, seed_row):
    h = N * IH
    world = World(h, W, p)
    world.add_pattern(ORBIUM, seed_row, W // 2 - 10)
    masses = []
    for _ in range(steps):
        masses.append(world.mass())
        world.step()
    return np.array(masses), world.A


def run_ring(p, N, IH, W, steps, seed_row):
    R = p.R
    taps, _ = build_taps(p)
    strips = [np.zeros((IH, W)) for _ in range(N)]
    # place Orbium spanning wherever seed_row lands
    s_idx, s_off = divmod(seed_row, IH)
    for r in range(20):
        rr = s_off + r
        si = s_idx + (rr // IH)
        rr = rr % IH
        if si >= N:
            continue
        strips[si][rr, W // 2 - 10:W // 2 + 10] = ORBIUM[r]

    masses = []
    com_rows = []
    for _ in range(steps):
        # global mass + COM (for boundary-crossing check)
        full = np.vstack(strips)
        masses.append(full.sum())
        m = full.sum()
        if m > 1e-6:
            ys = np.arange(full.shape[0])[:, None]
            com_rows.append(float((ys * full).sum() / m))

        # exchange uint8 halos around the ring
        top_edges = [np.round(s[0:R, :] * 255) / 255.0 for s in strips]      # each strip's top edge
        bot_edges = [np.round(s[IH - R:IH, :] * 255) / 255.0 for s in strips]  # bottom edge
        new = []
        for i in range(N):
            top_halo = bot_edges[(i - 1) % N]   # from strip above's bottom edge
            bot_halo = top_edges[(i + 1) % N]   # from strip below's top edge
            padded = np.vstack([top_halo, strips[i], bot_halo])
            U = np.zeros((IH, W))
            for (ky, kx, wt) in taps:
                U += wt * np.roll(padded[R + ky:R + ky + IH, :], -kx, axis=1)
            new.append(np.clip(strips[i] + (1.0 / p.T) * growth(U, p), 0.0, 1.0))
        strips = new
    return np.array(masses), com_rows, np.vstack(strips)


def main() -> None:
    p = LeniaParams()
    N, IH, W, steps = 10, 40, 128, 200
    seed_row = 7 * IH + 20    # start near a strip boundary so it must cross one

    print("=== Distributed halo-exchange ring vs monolith ===")
    mono_m, _ = run_monolith(p, N, IH, W, steps, seed_row)
    ring_m, com_rows, _ = run_ring(p, N, IH, W, steps, seed_row)

    # (a) alive + tracks monolith mass (loosely, due to uint8 halo quantization)
    alive = ring_m[-1] > 0.2 * ring_m[0] and ring_m[-1] < 5 * ring_m[0]
    rel = np.abs(ring_m - mono_m) / np.maximum(mono_m, 1e-6)
    tracks = float(np.median(rel)) < 0.15
    # (b) glider crosses strip boundaries: global COM row sweeps a wide range
    span = (max(com_rows) - min(com_rows)) if com_rows else 0.0
    crossed = span > IH   # moved across at least a full strip height

    print(f"  ring mass {ring_m[0]:.1f} -> {ring_m[-1]:.1f}  (monolith {mono_m[-1]:.1f})")
    print(f"  median mass tracking error = {np.median(rel) * 100:.1f}%   tracks={tracks}")
    print(f"  global COM row span = {span:.1f} cells (>{IH} = crossed boundaries: {crossed})")
    ok = alive and tracks and crossed
    print(f"  RESULT: {'PASS - distributed ring lives, tracks, and crosses strips' if ok else 'FAIL'}")
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
