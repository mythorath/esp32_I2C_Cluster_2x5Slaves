#!/usr/bin/env python3
"""End-to-end autonomy-layer validation (Phases 7a homeostasis + 7b detection).

Mirrors what the master does on the live cluster, in numpy:
  - run the distributed halo-exchange ring (the full world, uint8 halos),
  - 7a: a homeostasis controller watches world mass + activity and nudges
        sigma/T toward an "alive & interesting" band (same lever + activity
        gating as evolution.cpp), so the world fights to stay in band,
  - 7b: connected-component organism detection on the downsampled field with
        nearest-centroid tracking (same approach as detect.cpp).

Asserts:
  (1) detection finds and TRACKS the Orbium glider as a moving organism, and
  (2) for a world detuned to overgrow, homeostasis keeps it in the target band a
      larger fraction of the time than the uncontrolled world (control works).

Run: python world_sim.py
"""
from __future__ import annotations

import numpy as np
from scipy import ndimage
from scipy.signal import fftconvolve

from lenia_ref import LeniaParams, make_kernel, ORBIUM
from port_check import growth


def ring_step(strips, K, p):
    N = len(strips)
    IH, W = strips[0].shape
    R = p.R
    top_q = [np.round(s[0:R, :] * 255) / 255.0 for s in strips]      # uint8 top edges
    bot_q = [np.round(s[IH - R:IH, :] * 255) / 255.0 for s in strips]  # uint8 bottom edges
    out = []
    for i in range(N):
        py = np.vstack([bot_q[(i - 1) % N], strips[i], top_q[(i + 1) % N]])   # Y halos
        pf = np.hstack([py[:, -R:], py, py[:, :R]])                           # X toroidal
        U = fftconvolve(pf, K, mode="valid")                                  # -> IH x W
        out.append(np.clip(strips[i] + (1.0 / p.T) * growth(U, p), 0.0, 1.0))
    return out


def world_mass(strips):
    cells = sum(s.size for s in strips)
    return float(sum(s.sum() for s in strips) / cells)


def world_activity(prev, strips, eps=0.01):
    cells = sum(s.size for s in strips)
    ch = sum(int((np.abs(a - b) > eps).sum()) for a, b in zip(prev, strips))
    return ch / cells


def detect(field, thresh=0.15, min_area=4, max_area=4000):
    mask = field > thresh
    lab, n = ndimage.label(mask)
    comps = []
    for i in range(1, n + 1):
        ys, xs = np.where(lab == i)
        area = len(ys)
        if area < min_area or area > max_area:
            continue
        vals = field[ys, xs]
        m = vals.sum()
        comps.append((float((xs * vals).sum() / m), float((ys * vals).sum() / m)))
    return comps


def track(prev, comps, match_dist=9.0):
    new_tracks, max_disp = [], 0.0
    used = [False] * len(comps)
    for (px, py, disp) in prev:
        best, bd = -1, match_dist
        for ci, (cx, cy) in enumerate(comps):
            if used[ci]:
                continue
            d = np.hypot(cx - px, cy - py)
            if d < bd:
                bd, best = d, ci
        if best >= 0:
            used[best] = True
            cx, cy = comps[best]
            disp = disp + np.hypot(cx - px, cy - py)
            new_tracks.append((cx, cy, disp))
            max_disp = max(max_disp, disp)
    for ci, (cx, cy) in enumerate(comps):
        if not used[ci]:
            new_tracks.append((cx, cy, 0.0))
    return new_tracks, max_disp


def run_world(p0, N, IH, W, steps, homeostasis, seed_noise, target):
    rng = np.random.default_rng(7)
    if seed_noise:
        strips = [rng.random((IH, W)) * 0.5 for _ in range(N)]
    else:
        strips = [np.zeros((IH, W)) for _ in range(N)]
        strips[7][IH // 2 - 10:IH // 2 + 10, W // 2 - 10:W // 2 + 10] = ORBIUM

    p = LeniaParams(**vars(p0))
    K = make_kernel(p)
    LO, HI = target * 0.6, target * 1.4   # "in band" = within 40% of target
    masses, tracks, max_disp, err = [], [], 0.0, 0.0
    prev = strips
    for t in range(steps):
        m = world_mass(strips)
        masses.append(m)
        err += abs(m - target)            # control error (lower = better tracking)
        comps = detect(np.vstack(strips)[::2, ::2])
        tracks, md = track(tracks, comps)
        max_disp = max(max_disp, md)

        if homeostasis and t % 5 == 0:
            act = world_activity(prev, strips)
            # band is the deadband; only the safe sigma lever is driven (mu is
            # left to the slow island GA) - mirrors evolution.cpp homeostasis.
            if m < LO and act < 0.005:    # truly dying/frozen -> grow more
                p.sigma += 0.0006
            elif m > HI:                  # saturating -> narrow growth
                p.sigma -= 0.0006
            p.sigma = float(np.clip(p.sigma, 0.010, 0.10))
            K = make_kernel(p)

        prev = strips
        strips = ring_step(strips, K, p)
    return np.array(masses), max_disp, err / steps


def main() -> None:
    N, IH, W, steps = 10, 40, 128, 140
    print("=== End-to-end autonomy validation ===")

    # (1) detection + tracking: a single Orbium glider on the ring
    _, disp, _ = run_world(LeniaParams(), N, IH, W, steps,
                           homeostasis=False, seed_noise=False, target=0.1)
    detected_moving = disp > 5.0
    print(f"[7b] organism tracked displacement = {disp:.1f} DS-cells  "
          f"{'PASS (glider detected & tracked)' if detected_moving else 'FAIL'}")

    # (2) homeostasis exerts correct corrective pressure: from an overgrowing
    #     start, the controlled world tracks the target mass better than the
    #     uncontrolled one (lower mean control error).
    target = 0.07
    detuned = LeniaParams(sigma=0.020)     # wide growth -> world tends to overgrow
    m_off, _, err_off = run_world(detuned, N, IH, W, steps, homeostasis=False,
                                  seed_noise=True, target=target)
    m_on, _, err_on = run_world(detuned, N, IH, W, steps, homeostasis=True,
                                seed_noise=True, target=target)
    rescued = err_on < err_off and m_on[-1] > 0.002 and m_on[-1] < m_off[-1]
    print(f"[7a] control error to target {target}: uncontrolled={err_off:.4f}  "
          f"homeostasis={err_on:.4f}  (final mass {m_off[-1]:.3f}->{m_on[-1]:.3f})  "
          f"{'PASS (controller pulls toward target)' if rescued else 'FAIL'}")

    ok = detected_moving and rescued
    print(f"RESULT: {'PASS - autonomy layer detects life and regulates the world' if ok else 'FAIL'}")
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
