#!/usr/bin/env python3
"""Validate the FIRMWARE algorithm choices against the reference (Phase 2/3).

The on-device engine differs from the clean reference in three ways that could
each silently kill life. This script mirrors those choices in numpy and proves
they don't, so we trust the C++ before flashing:

  1. Sparse kernel taps (skip the zero region of the annulus) instead of a dense
     27x27 / FFT convolution                       -> matches dense to ~1e-6.
  2. uint8-quantized halos exchanged across strip boundaries each generation
     (1 byte per cell instead of float)            -> Orbium still lives+glides.
  3. Fixed-point bank: uint16 state, Q16 kernel, 1024-entry growth LUT, int64
     accumulate (ESP32-C3)                          -> blockier but still alive.

Run: python port_check.py
"""
from __future__ import annotations

import numpy as np

from lenia_ref import LeniaParams, bell, make_kernel, ORBIUM


# --------------------------------------------------------------------------
# Shared: sparse tap list (mirrors FloatPolicy.configure / FixedPolicy.configure)
# --------------------------------------------------------------------------
def build_taps(p: LeniaParams, beta=(1.0,)):
    R, B = p.R, len(beta)
    taps = []
    total = 0.0
    for ky in range(-R, R + 1):
        for kx in range(-R, R + 1):
            dist = np.hypot(ky, kx) / R
            if dist >= 1.0:
                continue
            rb = dist * B
            shell = min(int(rb), B - 1)
            frac = rb - shell
            w = beta[shell] * bell(np.array(frac), p.mu_k, p.sigma_k)
            w = float(w)
            if w <= 1e-6:
                continue
            taps.append((ky, kx, w))
            total += w
    return [(ky, kx, w / total) for (ky, kx, w) in taps], total


def growth(u, p: LeniaParams):
    return 2.0 * bell(u, p.mu, p.sigma) - 1.0


# --------------------------------------------------------------------------
# Check 1: sparse taps == dense FFT kernel
# --------------------------------------------------------------------------
def check_sparse_taps() -> bool:
    p = LeniaParams()
    h = w = 64
    rng = np.random.default_rng(1)
    A = rng.random((h, w))

    K = make_kernel(p)
    kfull = np.zeros((h, w))
    ks = K.shape[0]
    cy, cx, r = h // 2, w // 2, ks // 2
    kfull[cy - r:cy + r + 1, cx - r:cx + r + 1] = K
    dense = np.real(np.fft.ifft2(np.fft.fft2(A) * np.fft.fft2(np.fft.fftshift(kfull))))

    taps, _ = build_taps(p)
    sparse = np.zeros_like(A)
    for (ky, kx, wt) in taps:
        sparse += wt * np.roll(np.roll(A, ky, axis=0), kx, axis=1)

    err = float(np.abs(dense - sparse).max())
    print(f"[1] sparse-vs-dense max abs err = {err:.2e}  (taps={len(taps)})")
    return err < 1e-6


# --------------------------------------------------------------------------
# Check 2: float strip with uint8-quantized self-wrap halos keeps Orbium alive
# --------------------------------------------------------------------------
def _com(field):
    m = field.sum()
    if m < 1e-6:
        return float("nan"), float("nan")
    ys, xs = np.mgrid[0:field.shape[0], 0:field.shape[1]]
    return float((ys * field).sum() / m), float((xs * field).sum() / m)


def check_quantized_halo_float(steps: int = 220) -> bool:
    p = LeniaParams()
    R, IH, W = p.R, 40, 128
    taps, _ = build_taps(p)
    A = np.zeros((IH, W))
    A[IH // 2 - 10:IH // 2 + 10, W // 2 - 10:W // 2 + 10] = ORBIUM

    m0 = A.sum()
    cy0, cx0 = _com(A)
    for _ in range(steps):
        # build padded field: halos are uint8-quantized opposite edges (self-wrap)
        top_halo = np.round(A[IH - R:IH, :] * 255) / 255.0      # bottom edge -> top halo
        bot_halo = np.round(A[0:R, :] * 255) / 255.0            # top edge -> bottom halo
        padded = np.vstack([top_halo, A, bot_halo])             # (IH+2R, W)
        U = np.zeros((IH, W))
        for (ky, kx, wt) in taps:
            U += wt * np.roll(padded[R + ky:R + ky + IH, :], -kx, axis=1)
        A = np.clip(A + (1.0 / p.T) * growth(U, p), 0.0, 1.0)

    m1 = A.sum()
    cy1, cx1 = _com(A)
    dy = (cy1 - cy0 + IH / 2) % IH - IH / 2
    dx = (cx1 - cx0 + W / 2) % W - W / 2
    drift = float(np.hypot(dy, dx))
    ok = 0.1 * m0 < m1 < 10 * m0 and drift > 2.0
    print(f"[2] float strip + uint8 halo: mass {m0:.1f}->{m1:.1f} drift={drift:.2f}  "
          f"{'PASS' if ok else 'FAIL'}")
    return ok


# --------------------------------------------------------------------------
# Check 3: fixed-point bank (uint16 + Q16 kernel + 1024 LUT + int64 acc)
# --------------------------------------------------------------------------
def check_fixed_point(steps: int = 220) -> bool:
    p = LeniaParams()
    R, IH, W = p.R, 40, 128
    taps_f, _ = build_taps(p)
    # Q16 kernel weights (sum ~= 65536)
    taps_q = [(ky, kx, int(round(wt * 65536))) for (ky, kx, wt) in taps_f]
    # 1024-entry growth-delta LUT over quantized U, baking in 1/T and u16 scale.
    lut = np.zeros(1024, dtype=np.int32)
    for i in range(1024):
        u = i / 1023.0
        g = 2.0 * float(bell(np.array(u), p.mu, p.sigma)) - 1.0
        lut[i] = int(round((1.0 / p.T) * g * 65535.0))

    A = np.zeros((IH, W), dtype=np.uint16)
    seed = np.round(ORBIUM * 65535).astype(np.uint16)
    A[IH // 2 - 10:IH // 2 + 10, W // 2 - 10:W // 2 + 10] = seed

    def mass(a):
        return float(a.astype(np.float64).sum() / 65535.0)

    m0 = mass(A)
    for _ in range(steps):
        top_halo = A[IH - R:IH, :]
        bot_halo = A[0:R, :]
        padded = np.vstack([top_halo, A, bot_halo]).astype(np.int64)
        acc = np.zeros((IH, W), dtype=np.int64)
        for (ky, kx, wq) in taps_q:
            acc += wq * np.roll(padded[R + ky:R + ky + IH, :], -kx, axis=1)
        U = (acc >> 16)                                  # back to u16 scale (0..65535)
        U = np.clip(U, 0, 65535).astype(np.int64)
        idx = (U >> 6).astype(np.int64)                  # 0..1023
        idx = np.clip(idx, 0, 1023)
        delta = lut[idx]
        newA = A.astype(np.int64) + delta
        A = np.clip(newA, 0, 65535).astype(np.uint16)

    m1 = mass(A)
    ok = 0.1 * m0 < m1 < 10 * m0
    print(f"[3] fixed-point strip:        mass {m0:.1f}->{m1:.1f}  "
          f"{'PASS (blockier but alive)' if ok else 'FAIL'}")
    return ok


def main() -> None:
    print("=== Firmware algorithm port-check (numpy mirror of the C++ policies) ===")
    ok = True
    ok &= check_sparse_taps()
    ok &= check_quantized_halo_float()
    ok &= check_fixed_point()
    print(f"RESULT: {'PASS - firmware math choices preserve life' if ok else 'FAIL'}")
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
