#!/usr/bin/env python3
"""Chimera Lenia - 2-channel (multi-species) reference simulator (Phase 0).

Single-channel Lenia gives isolated gliders that ignore each other. Two coupled
channels give genuine inter-species interaction. Each channel self-sustains
Orbium-class gliders (so life persists), plus a weak SIGNED cross-coupling:

    species 0 = "prey"      species 1 = "predator"
    U_ij = K_ij * A_j  (toroidal convolution; K_ij a normalized Gaussian shell)

    dA0 = dt * ( Gself(U00)  +  w_prey * Gcross(U01) )   # w_prey < 0 -> flee/die near predators
    dA1 = dt * ( Gself(U11)  +  w_pred * Gcross(U10) )   # w_pred > 0 -> grow toward prey
    A_i = clip(A_i + dA_i, 0, 1)

This file locks the two bank "personalities" the firmware will seed:
  - INSTINCT (Bank A / C3): fast dt, strong coupling -> twitchy, reactive chases.
  - MEMORY   (Bank B / S3): slow dt, gentle coupling -> calm, persistent drift.

It is the desktop gate before any of this touches a microcontroller.

Usage:
    python multichannel_ref.py            # headless self-test (asserts interaction)
    python multichannel_ref.py --animate  # watch the two species interact
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field

import numpy as np

from lenia_ref import LeniaParams, make_kernel, bell, ORBIUM


# --------------------------------------------------------------------------
# Two-species genome. Self terms are Orbium-class; cross terms are wider
# "sensing" shells with signed weights. Keep in sync with lib/shared/genome.h.
# --------------------------------------------------------------------------
@dataclass
class MultiGenome:
    R: int = 13
    T: float = 10.0
    # self dynamics (both channels use Orbium-class self-growth)
    mu: float = 0.15
    sigma: float = 0.015
    mu_k: float = 0.5
    sigma_k: float = 0.15
    # cross dynamics: a wider sensing shell + a PRESENCE bump (0 when the other
    # species is absent, peaks at sensed density ~mu_c). Using a plain bell (not
    # 2*bell-1) keeps the cross term ~0 in empty space, so it perturbs gliders
    # only where species actually overlap instead of biasing the whole field.
    mu_kc: float = 0.0     # center-weighted disk -> senses local presence across R
    sigma_kc: float = 0.30
    mu_c: float = 0.20
    sigma_c: float = 0.05
    # signed interaction weights (fraction of self growth)
    w_prey: float = -0.18   # predators suppress prey
    w_pred: float = +0.22   # prey feed predators


# Locked bank presets. Kept in sync with lib/shared/genome.h
# (instinctGenome / memoryGenome) - these are the authoritative live values.
INSTINCT = MultiGenome(T=5.5,  w_prey=-0.30, w_pred=+0.36)   # fast, reactive
MEMORY   = MultiGenome(T=10.0, w_prey=-0.18, w_pred=+0.22)   # calmer, persistent


def _fft_kernel(K: np.ndarray, h: int, w: int) -> np.ndarray:
    kfull = np.zeros((h, w))
    ks = K.shape[0]
    cy, cx, r = h // 2, w // 2, ks // 2
    kfull[cy - r:cy + r + 1, cx - r:cx + r + 1] = K
    return np.fft.fft2(np.fft.fftshift(kfull))


class MultiWorld:
    def __init__(self, h: int, w: int, g: MultiGenome):
        self.h, self.w, self.g = h, w, g
        self.A = [np.zeros((h, w)), np.zeros((h, w))]
        self_p = LeniaParams(R=g.R, mu=g.mu, sigma=g.sigma, mu_k=g.mu_k, sigma_k=g.sigma_k)
        cross_p = LeniaParams(R=g.R, mu=g.mu, sigma=g.sigma, mu_k=g.mu_kc, sigma_k=g.sigma_kc)
        self._fk_self = _fft_kernel(make_kernel(self_p), h, w)
        self._fk_cross = _fft_kernel(make_kernel(cross_p), h, w)
        self.gen = 0

    def _conv(self, a, fk):
        return np.real(np.fft.ifft2(np.fft.fft2(a) * fk))

    def step(self, coupled: bool = True):
        g = self.g
        A0, A1 = self.A
        u00 = self._conv(A0, self._fk_self)
        u11 = self._conv(A1, self._fk_self)
        g0 = 2.0 * bell(u00, g.mu, g.sigma) - 1.0
        g1 = 2.0 * bell(u11, g.mu, g.sigma) - 1.0
        if coupled:
            u01 = self._conv(A1, self._fk_cross)   # predator field sensed by prey
            u10 = self._conv(A0, self._fk_cross)   # prey field sensed by predator
            # presence bump (~0 when other species absent) -> localized perturbation
            g0 = g0 + g.w_prey * bell(u01, g.mu_c, g.sigma_c)
            g1 = g1 + g.w_pred * bell(u10, g.mu_c, g.sigma_c)
        self.A[0] = np.clip(A0 + (1.0 / g.T) * g0, 0.0, 1.0)
        self.A[1] = np.clip(A1 + (1.0 / g.T) * g1, 0.0, 1.0)
        self.gen += 1

    def seed(self, overlap: bool = True):
        h, w = self.h, self.w
        if overlap:
            # overlapping so the species interact immediately (deterministic test)
            self._stamp(0, h // 2 - 4, w // 2 - 4)
            self._stamp(1, h // 2 + 4, w // 2 + 4)
        else:
            # far apart -> independent drift (chase demo / animate)
            self._stamp(0, h // 2 - 22, w // 2 - 22)
            self._stamp(1, h // 2 + 6, w // 2 + 6)

    def seed_noise(self, rng: np.random.Generator):
        # independent soft random fields per channel -> stay spatially mixed so
        # the cross-coupling acts continuously (robust mechanism test).
        for ch in range(2):
            f = rng.random((self.h, self.w))
            # smooth it a little so structures (not pixel hash) form
            fk = np.fft.fft2(f)
            yy, xx = np.mgrid[0:self.h, 0:self.w]
            lp = np.exp(-((np.minimum(yy, self.h - yy) ** 2 + np.minimum(xx, self.w - xx) ** 2)) / (2 * 4.0 ** 2))
            self.A[ch] = np.clip(np.real(np.fft.ifft2(fk * np.fft.fft2(lp))) , 0, None)
            mx = self.A[ch].max()
            if mx > 0:
                self.A[ch] *= 0.5 / mx

    def _stamp(self, ch, top, left):
        for r in range(20):
            rr = (top + r) % self.h
            for c in range(20):
                cc = (left + c) % self.w
                self.A[ch][rr, cc] = ORBIUM[r][c]

    def mass(self, ch):
        return float(self.A[ch].sum())

    def com(self, ch):
        a = self.A[ch]
        m = a.sum()
        if m < 1e-6:
            return (float("nan"), float("nan"))
        ys, xs = np.mgrid[0:self.h, 0:self.w]
        return (float((ys * a).sum() / m), float((xs * a).sum() / m))


def _torus_dist(c0, c1, h, w):
    dy = (c0[0] - c1[0] + h / 2) % h - h / 2
    dx = (c0[1] - c1[1] + w / 2) % w - w / 2
    return float(np.hypot(dy, dx))


def _corr(a, b):
    av, bv = a.ravel(), b.ravel()
    if av.std() < 1e-6 or bv.std() < 1e-6:
        return 0.0
    return float(np.corrcoef(av, bv)[0, 1])


def run(g: MultiGenome, steps: int, coupled: bool, h=96, w=96):
    world = MultiWorld(h, w, g)
    world.seed(overlap=True)         # two adjacent gliders -> sustained sensing overlap
    m00, m10 = world.mass(0), world.mass(1)
    for _ in range(steps):
        world.step(coupled=coupled)
    return {
        "A0": world.A[0].copy(), "A1": world.A[1].copy(),
        "m0": world.mass(0), "m1": world.mass(1),
        "alive": world.mass(0) > 0.15 * m00 and world.mass(1) > 0.15 * m10
                 and world.mass(0) < 8 * m00 and world.mass(1) < 8 * m10,
    }


def self_test() -> bool:
    print("=== 2-channel (multi-species) interaction self-test ===")
    ok = True
    for name, g in (("instinct", INSTINCT), ("memory", MEMORY)):
        # identical initial conditions; coupling is the only difference.
        coup = run(g, 140, coupled=True)
        unco = run(g, 140, coupled=False)
        # interaction proof: with both species alive, the cross-coupling
        # measurably redirects/reshapes the organisms -> the coupled field
        # diverges from the uncoupled baseline (per channel, normalized L2).
        d0 = np.linalg.norm(unco["A0"]) + 1e-6
        d1 = np.linalg.norm(unco["A1"]) + 1e-6
        ch0 = float(np.linalg.norm(coup["A0"] - unco["A0"]) / d0)
        ch1 = float(np.linalg.norm(coup["A1"] - unco["A1"]) / d1)
        interacts = coup["alive"] and max(ch0, ch1) > 0.15
        ok &= interacts
        print(f"  {name:8s}: alive={coup['alive']} mass({coup['m0']:.0f},{coup['m1']:.0f})  "
              f"field-change prey={ch0*100:.0f}% pred={ch1*100:.0f}%  "
              f"{'PASS' if interacts else 'FAIL'}")
    print(f"RESULT: {'PASS - coupling redirects the species while life persists' if ok else 'FAIL'}")
    return ok


def animate():
    import matplotlib.pyplot as plt
    import matplotlib.animation as animation
    world = MultiWorld(120, 120, INSTINCT)
    world.seed()
    fig, ax = plt.subplots(figsize=(5, 5))
    rgb = np.zeros((120, 120, 3))
    im = ax.imshow(rgb)
    ax.set_title("Chimera multi-species (green=prey, magenta=predator)")
    ax.axis("off")

    def update(_):
        world.step(coupled=True)
        rgb[..., 1] = world.A[0]                 # prey -> green
        rgb[..., 0] = world.A[1]                 # predator -> red+blue = magenta
        rgb[..., 2] = world.A[1]
        im.set_data(np.clip(rgb, 0, 1))
        ax.set_title(f"gen {world.gen}  prey {world.mass(0):.0f}  pred {world.mass(1):.0f}")
        return [im]

    _a = animation.FuncAnimation(fig, update, frames=800, interval=30, blit=False)
    plt.show()


def main():
    ap = argparse.ArgumentParser(description="Chimera multi-species reference")
    ap.add_argument("--animate", action="store_true")
    args = ap.parse_args()
    if args.animate:
        animate()
    else:
        raise SystemExit(0 if self_test() else 1)


if __name__ == "__main__":
    main()
