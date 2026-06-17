#!/usr/bin/env python3
"""Chimera Lenia - desktop reference simulator (Phase 1).

Validate the Lenia math, lock in Orbium-class parameters, and produce
ground-truth field/halo dumps BEFORE any of it touches a microcontroller.
The firmware (node_s3 float, node_c3 fixed-point) must reproduce what this
file does; phase 4's two-strip halo exchange is validated against
``export_strip_reference`` below.

Standard Lenia (Chan 2019), torus, single Gaussian-shell kernel:
    K(r)      = bell(r, mu_k, sigma_k) for r < 1, normalized so sum(K)=1
    U         = K * A           (toroidal 2D convolution)
    G(u)      = 2*bell(u, mu, sigma) - 1
    A_new     = clip(A + (1/T) * G(U), 0, 1)

with the Orbium params: R=13, T=10, mu=0.15, sigma=0.015,
mu_k=0.5, sigma_k=0.15.

Usage:
    python lenia_ref.py                 # headless self-test (asserts life)
    python lenia_ref.py --animate       # matplotlib animation of the glider
    python lenia_ref.py --export out/   # write field + halo reference dumps
"""
from __future__ import annotations

import argparse
import json
import os
from dataclasses import dataclass, asdict

import numpy as np


# --------------------------------------------------------------------------
# Parameters (the locked v1 genome - keep in sync with lib/shared/genome.h)
# --------------------------------------------------------------------------
@dataclass
class LeniaParams:
    R: int = 13          # kernel radius (cells)
    T: float = 10.0      # time resolution; dt = 1/T
    mu: float = 0.15     # growth center
    sigma: float = 0.015  # growth width
    mu_k: float = 0.5    # kernel shell center (fraction of R)
    sigma_k: float = 0.15  # kernel shell width


# --------------------------------------------------------------------------
# Canonical Orbium glider (20x20), Bert Chan / Lenia reference.
# Pairs with the Gaussian-shell kernel + Orbium params above.
# --------------------------------------------------------------------------
ORBIUM = np.array([
    [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.1, 0.14, 0.1, 0.0, 0.0, 0.03, 0.03, 0.0, 0.0, 0.3, 0.0, 0.0, 0.0, 0.0],
    [0.0, 0.0, 0.0, 0.0, 0.0, 0.08, 0.24, 0.3, 0.3, 0.18, 0.14, 0.15, 0.16, 0.15, 0.09, 0.2, 0.0, 0.0, 0.0, 0.0],
    [0.0, 0.0, 0.0, 0.0, 0.0, 0.15, 0.34, 0.44, 0.46, 0.38, 0.18, 0.14, 0.11, 0.13, 0.19, 0.18, 0.45, 0.0, 0.0, 0.0],
    [0.0, 0.0, 0.0, 0.0, 0.06, 0.13, 0.39, 0.5, 0.5, 0.37, 0.06, 0.0, 0.0, 0.0, 0.02, 0.16, 0.68, 0.0, 0.0, 0.0],
    [0.0, 0.0, 0.0, 0.11, 0.17, 0.17, 0.33, 0.4, 0.38, 0.28, 0.14, 0.0, 0.0, 0.0, 0.0, 0.0, 0.18, 0.42, 0.0, 0.0],
    [0.0, 0.0, 0.09, 0.18, 0.13, 0.06, 0.08, 0.26, 0.32, 0.32, 0.27, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.82, 0.0, 0.0],
    [0.27, 0.0, 0.16, 0.12, 0.0, 0.0, 0.0, 0.25, 0.38, 0.44, 0.45, 0.34, 0.0, 0.0, 0.0, 0.0, 0.0, 0.22, 0.17, 0.0],
    [0.0, 0.07, 0.2, 0.02, 0.0, 0.0, 0.0, 0.31, 0.48, 0.57, 0.6, 0.57, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.49, 0.0],
    [0.0, 0.59, 0.19, 0.0, 0.0, 0.0, 0.0, 0.2, 0.57, 0.69, 0.76, 0.76, 0.49, 0.0, 0.0, 0.0, 0.0, 0.0, 0.36, 0.0],
    [0.0, 0.58, 0.19, 0.0, 0.0, 0.0, 0.0, 0.0, 0.67, 0.83, 0.9, 0.92, 0.87, 0.12, 0.0, 0.0, 0.0, 0.0, 0.22, 0.07],
    [0.0, 0.0, 0.46, 0.0, 0.0, 0.0, 0.0, 0.0, 0.7, 0.93, 1.0, 1.0, 1.0, 0.61, 0.0, 0.0, 0.0, 0.0, 0.18, 0.11],
    [0.0, 0.0, 0.82, 0.0, 0.0, 0.0, 0.0, 0.0, 0.47, 1.0, 1.0, 0.98, 1.0, 0.96, 0.27, 0.0, 0.0, 0.0, 0.19, 0.1],
    [0.0, 0.0, 0.46, 0.0, 0.0, 0.0, 0.0, 0.0, 0.25, 1.0, 1.0, 0.84, 0.92, 0.97, 0.54, 0.14, 0.04, 0.1, 0.21, 0.05],
    [0.0, 0.0, 0.0, 0.4, 0.0, 0.0, 0.0, 0.0, 0.09, 0.8, 1.0, 0.82, 0.8, 0.85, 0.63, 0.31, 0.18, 0.19, 0.2, 0.01],
    [0.0, 0.0, 0.0, 0.36, 0.1, 0.0, 0.0, 0.0, 0.05, 0.54, 0.86, 0.79, 0.74, 0.72, 0.6, 0.39, 0.28, 0.24, 0.13, 0.0],
    [0.0, 0.0, 0.0, 0.01, 0.3, 0.07, 0.0, 0.0, 0.08, 0.36, 0.64, 0.7, 0.64, 0.6, 0.51, 0.39, 0.29, 0.19, 0.04, 0.0],
    [0.0, 0.0, 0.0, 0.0, 0.1, 0.24, 0.14, 0.1, 0.15, 0.29, 0.45, 0.53, 0.52, 0.46, 0.4, 0.31, 0.21, 0.08, 0.0, 0.0],
    [0.0, 0.0, 0.0, 0.0, 0.0, 0.08, 0.21, 0.21, 0.22, 0.29, 0.36, 0.39, 0.37, 0.33, 0.26, 0.18, 0.09, 0.0, 0.0, 0.0],
    [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.03, 0.13, 0.19, 0.22, 0.24, 0.24, 0.23, 0.18, 0.13, 0.05, 0.0, 0.0, 0.0, 0.0],
    [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.02, 0.06, 0.08, 0.09, 0.07, 0.05, 0.01, 0.0, 0.0, 0.0, 0.0, 0.0],
], dtype=np.float64)


def bell(x: np.ndarray, m: float, s: float) -> np.ndarray:
    """Gaussian bell, exp(-((x-m)/s)^2 / 2)."""
    return np.exp(-(((x - m) / s) ** 2) / 2.0)


def make_kernel(p: LeniaParams) -> np.ndarray:
    """Radial Gaussian-shell kernel of support radius R, normalized to sum 1."""
    rng = np.arange(-p.R, p.R + 1)
    xx, yy = np.meshgrid(rng, rng)
    dist = np.sqrt(xx ** 2 + yy ** 2) / p.R
    k = (dist < 1.0) * bell(dist, p.mu_k, p.sigma_k)
    total = k.sum()
    if total == 0:
        raise ValueError("Degenerate kernel (sum == 0)")
    return k / total


class World:
    """A toroidal Lenia world with FFT-accelerated convolution."""

    def __init__(self, h: int, w: int, params: LeniaParams | None = None):
        self.h = h
        self.w = w
        self.p = params or LeniaParams()
        self.A = np.zeros((h, w), dtype=np.float64)
        self.kernel = make_kernel(self.p)
        self._fk = self._kernel_fft()
        self.gen = 0

    def _kernel_fft(self) -> np.ndarray:
        k_full = np.zeros((self.h, self.w), dtype=np.float64)
        ks = self.kernel.shape[0]
        # place kernel centered, then shift origin to (0,0) for FFT convolution
        cy, cx = self.h // 2, self.w // 2
        r = ks // 2
        k_full[cy - r:cy + r + 1, cx - r:cx + r + 1] = self.kernel
        return np.fft.fft2(np.fft.fftshift(k_full))

    def potential(self, field: np.ndarray | None = None) -> np.ndarray:
        a = self.A if field is None else field
        return np.real(np.fft.ifft2(np.fft.fft2(a) * self._fk))

    def growth(self, u: np.ndarray) -> np.ndarray:
        return 2.0 * bell(u, self.p.mu, self.p.sigma) - 1.0

    def step(self) -> None:
        u = self.potential()
        self.A = np.clip(self.A + (1.0 / self.p.T) * self.growth(u), 0.0, 1.0)
        self.gen += 1

    def add_pattern(self, pattern: np.ndarray, top: int, left: int) -> None:
        ph, pw = pattern.shape
        self.A[top:top + ph, left:left + pw] = pattern

    def mass(self) -> float:
        return float(self.A.sum())

    def center_of_mass(self) -> tuple[float, float]:
        m = self.A.sum()
        if m <= 1e-9:
            return (float("nan"), float("nan"))
        ys, xs = np.mgrid[0:self.h, 0:self.w]
        return (float((ys * self.A).sum() / m), float((xs * self.A).sum() / m))


def seed_orbium(world: World, top: int | None = None, left: int | None = None) -> None:
    top = world.h // 2 - ORBIUM.shape[0] // 2 if top is None else top
    left = world.w // 2 - ORBIUM.shape[1] // 2 if left is None else left
    world.add_pattern(ORBIUM, top, left)


# --------------------------------------------------------------------------
# Self-test: confirm Orbium survives and glides (the phase-1 gate)
# --------------------------------------------------------------------------
def self_test(steps: int = 300, verbose: bool = True) -> bool:
    world = World(64, 64)
    seed_orbium(world)
    m0 = world.mass()
    com0 = world.center_of_mass()
    masses = [m0]
    for _ in range(steps):
        world.step()
        masses.append(world.mass())
    m1 = world.mass()
    com1 = world.center_of_mass()
    masses = np.array(masses)

    alive = m1 > 0.1 * m0                     # not dead
    bounded = m1 < 10.0 * m0                  # not exploded
    # COM drift on a torus: account for wrap by comparing displacement magnitude
    dy = (com1[0] - com0[0] + world.h / 2) % world.h - world.h / 2
    dx = (com1[1] - com0[1] + world.w / 2) % world.w - world.w / 2
    drift = float(np.hypot(dy, dx))
    moved = drift > 3.0                       # a glider, not a static blob
    stable_band = masses[steps // 2:].std() < 0.5 * masses[steps // 2:].mean()

    ok = alive and bounded and moved and stable_band
    if verbose:
        print("=== Chimera Lenia reference self-test ===")
        print(f"  steps           : {steps}")
        print(f"  mass start/end  : {m0:.2f} -> {m1:.2f}")
        print(f"  COM drift       : {drift:.2f} cells  (moved={moved})")
        print(f"  alive           : {alive}")
        print(f"  bounded         : {bounded}")
        print(f"  mass stable     : {stable_band}")
        print(f"  RESULT          : {'PASS - Orbium lives and glides' if ok else 'FAIL'}")
    return ok


def animate() -> None:
    import matplotlib.pyplot as plt
    import matplotlib.animation as animation

    world = World(96, 96)
    seed_orbium(world)
    fig, ax = plt.subplots(figsize=(5, 5))
    im = ax.imshow(world.A, cmap="viridis", vmin=0, vmax=1, interpolation="nearest")
    ax.set_title("Chimera Lenia - Orbium (reference)")
    ax.axis("off")

    def update(_frame):
        world.step()
        im.set_data(world.A)
        ax.set_title(f"gen {world.gen}  mass {world.mass():.1f}")
        return [im]

    _anim = animation.FuncAnimation(fig, update, frames=600, interval=30, blit=False)
    plt.show()


def export_strip_reference(out_dir: str, n_strips: int = 10, w: int = 128,
                           strip_h: int = 40, steps: int = 50) -> None:
    """Write ground-truth strip + halo dumps for hardware validation (phase 4).

    Produces, for a W x (n_strips*strip_h) torus seeded with Orbium:
      - field_genXXXX.npy : the full field each step
      - halos.json        : per-step, per-strip top/bottom halo rows (R deep),
                            quantized to uint8, so node firmware halo exchange
                            can be checked byte-for-byte against this.
    """
    os.makedirs(out_dir, exist_ok=True)
    p = LeniaParams()
    h = n_strips * strip_h
    world = World(h, w, p)
    seed_orbium(world, top=h // 2, left=w // 2 - ORBIUM.shape[1] // 2)

    halos = []
    for s in range(steps):
        np.save(os.path.join(out_dir, f"field_gen{s:04d}.npy"), world.A.astype(np.float32))
        step_halos = []
        for i in range(n_strips):
            y0 = i * strip_h
            top = world.A[y0:y0 + p.R, :]
            bottom = world.A[y0 + strip_h - p.R:y0 + strip_h, :]
            step_halos.append({
                "strip": i,
                "top_u8": (top * 255).round().astype(np.uint8).flatten().tolist(),
                "bottom_u8": (bottom * 255).round().astype(np.uint8).flatten().tolist(),
            })
        halos.append({"gen": s, "strips": step_halos})
        world.step()

    meta = {
        "params": asdict(p),
        "world": {"w": w, "h": h, "n_strips": n_strips, "strip_h": strip_h},
        "steps": steps,
    }
    with open(os.path.join(out_dir, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)
    with open(os.path.join(out_dir, "halos.json"), "w") as f:
        json.dump(halos, f)
    print(f"Exported {steps} gens of {n_strips}x{strip_h} strips (W={w}) to {out_dir}/")


def main() -> None:
    ap = argparse.ArgumentParser(description="Chimera Lenia reference simulator")
    ap.add_argument("--animate", action="store_true", help="show matplotlib animation")
    ap.add_argument("--export", metavar="DIR", help="export strip/halo reference dumps")
    ap.add_argument("--steps", type=int, default=300, help="self-test steps")
    args = ap.parse_args()

    if args.animate:
        animate()
    elif args.export:
        export_strip_reference(args.export, steps=args.steps)
    else:
        ok = self_test(steps=args.steps)
        raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
