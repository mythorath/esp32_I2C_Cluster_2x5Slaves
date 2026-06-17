#!/usr/bin/env python3
"""Chimera Lenia - fitness metric prototype (Phase 1).

Prototype the per-strip fitness used by the island-model evolution (sec 5b)
offline, so the firmware port has a reference to match. Fitness is a weighted
combination of:

  - persistence : temporal autocorrelation of the field over a window
                  ("is this structure persistent but moving?") - this is what
                  Bank B's PSRAM history ring buffer enables on-device.
  - motion      : center-of-mass drift per generation (a glider scores high,
                  a static blob scores ~0).
  - structure   : spatial entropy in a mid-band (neither empty nor full noise).

A living Orbium glider should clearly beat both an empty field and random
noise. This script asserts that ordering as its self-test.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass

import numpy as np

from lenia_ref import World, LeniaParams, seed_orbium


@dataclass
class FitnessWeights:
    # Motion is weighted highest: a directional glider is the clearest signal
    # of "interesting life" vs a persistent-but-static settled soup.
    persistence: float = 0.5
    motion: float = 2.0
    structure: float = 1.0


def spatial_entropy(field: np.ndarray, bins: int = 16) -> float:
    """Shannon entropy of the value histogram, normalized to [0,1].

    Peaks for mid-complexity fields; ~0 for empty/saturated, lower for pure
    noise once normalized against the mid-band target below.
    """
    hist, _ = np.histogram(field, bins=bins, range=(0.0, 1.0), density=False)
    p = hist.astype(np.float64)
    total = p.sum()
    if total == 0:
        return 0.0
    p /= total
    nz = p[p > 0]
    h = -(nz * np.log(nz)).sum()
    return float(h / np.log(bins))


def structure_score(field: np.ndarray, target: float = 0.12, width: float = 0.1) -> float:
    """Reward organism-sized occupancy via a Gaussian around `target`.

    A coherent organism occupies a small fraction of the strip; empty (occ->0)
    and noise/saturated (occ->1) fields both fall off the Gaussian and score
    near zero, while a localized structure near `target` occupancy scores ~1.
    """
    occ = float((field > 0.1).mean())
    return float(np.exp(-(((occ - target) / width) ** 2) / 2.0))


def temporal_autocorr(history: list[np.ndarray]) -> float:
    """Mean lag-1 correlation across the history window (persistence)."""
    if len(history) < 2:
        return 0.0
    cors = []
    for a, b in zip(history[:-1], history[1:]):
        av, bv = a.ravel(), b.ravel()
        if av.std() < 1e-6 or bv.std() < 1e-6:
            cors.append(0.0)
            continue
        cors.append(float(np.corrcoef(av, bv)[0, 1]))
    return float(np.clip(np.mean(cors), 0.0, 1.0))


def motion_score(coms: list[tuple[float, float]], h: int, w: int) -> float:
    """Directional motion: magnitude of the MEAN per-step velocity vector.

    A glider's per-step displacements all point the same way, so they sum
    coherently -> large mean vector. Random noise COM jitter cancels out ->
    near-zero mean vector. This is what distinguishes a directional organism
    from a churning soup that merely changes a lot. Torus-aware per step
    (each step's displacement is < grid/2, so no wrap ambiguity).
    """
    if len(coms) < 2:
        return 0.0
    vy, vx, n = 0.0, 0.0, 0
    for (y0, x0), (y1, x1) in zip(coms[:-1], coms[1:]):
        if any(np.isnan(v) for v in (y0, x0, y1, x1)):
            continue
        vy += (y1 - y0 + h / 2) % h - h / 2
        vx += (x1 - x0 + w / 2) % w - w / 2
        n += 1
    if n == 0:
        return 0.0
    speed = float(np.hypot(vy / n, vx / n))
    return float(1.0 - np.exp(-speed))     # 0 for static/jitter, ->1 for movers


def evaluate(world: World, window: int = 16, weights: FitnessWeights | None = None) -> dict:
    weights = weights or FitnessWeights()
    history: list[np.ndarray] = []
    coms: list[tuple[float, float]] = []
    for _ in range(window):
        history.append(world.A.copy())
        coms.append(world.center_of_mass())
        world.step()

    persistence = temporal_autocorr(history)
    motion = motion_score(coms, world.h, world.w)
    structure = float(np.mean([structure_score(f) for f in history]))

    fitness = (weights.persistence * persistence +
               weights.motion * motion +
               weights.structure * structure)
    return {
        "persistence": persistence,
        "motion": motion,
        "structure": structure,
        "fitness": fitness,
    }


def _orbium_world() -> World:
    w = World(64, 64)
    seed_orbium(w)
    return w


def _empty_world() -> World:
    return World(64, 64)


def _noise_world(seed: int = 0) -> World:
    rng = np.random.default_rng(seed)
    w = World(64, 64)
    w.A = rng.random((64, 64))
    return w


def self_test(verbose: bool = True) -> bool:
    orb = evaluate(_orbium_world())
    empty = evaluate(_empty_world())
    noise = evaluate(_noise_world())

    if verbose:
        print("=== Fitness probe ===")
        for name, r in (("orbium", orb), ("empty", empty), ("noise", noise)):
            print(f"  {name:7s}: persist={r['persistence']:.3f} "
                  f"motion={r['motion']:.3f} struct={r['structure']:.3f} "
                  f"=> fitness={r['fitness']:.3f}")

    ok = orb["fitness"] > empty["fitness"] and orb["fitness"] > noise["fitness"]
    if verbose:
        print(f"  RESULT : {'PASS - living glider scores highest' if ok else 'FAIL'}")
    return ok


def main() -> None:
    ap = argparse.ArgumentParser(description="Chimera Lenia fitness probe")
    ap.add_argument("--window", type=int, default=16)
    ap.parse_args()
    raise SystemExit(0 if self_test() else 1)


if __name__ == "__main__":
    main()
