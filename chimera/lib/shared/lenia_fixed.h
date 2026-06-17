// lenia_fixed.h - Bank A "instinct" policy: fixed-point (ESP32-C3 / RISC-V).
//
// State is uint16 (0..65535 == 0..1). The kernel is stored as Q16 weights
// (sum ~= 65536), convolution accumulates in int64, and growth is a precomputed
// 1024-entry LUT over the quantized potential U. No FPU pressure -> fast, but
// visibly blockier/quantized (the point of the heterogeneity). A small dither
// on the unit->fixed transcode (used at the seam) avoids banding; see master
// halo_router transcoding.
//
// Validated against the reference in tools/sim/port_check.py (check 3).
#pragma once

#include <stdint.h>
#include <math.h>
#include "protocol.h"
#include "genome.h"

namespace chimera {

class FixedPolicy {
public:
    using State = uint16_t;
    using Acc   = int64_t;

    static constexpr int MAX_TAPS = KERNEL_DIM * KERNEL_DIM;  // 729

    static inline State zero()    { return 0; }
    static inline Acc   zeroAcc() { return 0; }
    static inline float toUnit(State s)  { return s * (1.0f / 65535.0f); }
    static inline State fromUnit(float u) {
        int v = (int)(u * 65535.0f + 0.5f);
        return (State)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
    }

    int    nTaps = 0;
    int8_t tapDy[MAX_TAPS];
    int8_t tapDx[MAX_TAPS];
    int32_t tapW[MAX_TAPS];     // Q16

    void configure(const Genome& g) {
        const int R = KERNEL_R;
        const int B = g.n_beta < 1 ? 1 : g.n_beta;
        float raw[MAX_TAPS];
        int8_t dy[MAX_TAPS], dx[MAX_TAPS];
        int n = 0;
        float total = 0.0f;
        for (int ky = -R; ky <= R; ky++) {
            for (int kx = -R; kx <= R; kx++) {
                float dist = sqrtf((float)(ky * ky + kx * kx)) / R;
                if (dist >= 1.0f) continue;
                float rb = dist * B;
                int shell = (int)rb;
                if (shell >= B) shell = B - 1;
                float frac = rb - shell;
                float w = g.beta[shell] * bell_(frac, g.mu_k, g.sigma_k);
                if (w <= 1e-6f) continue;
                raw[n] = w; dy[n] = (int8_t)ky; dx[n] = (int8_t)kx; n++;
                total += w;
            }
        }
        if (total <= 0.0f) total = 1.0f;
        nTaps = n;
        for (int i = 0; i < n; i++) {
            tapW[i] = (int32_t)lroundf((raw[i] / total) * 65536.0f);
            tapDy[i] = dy[i]; tapDx[i] = dx[i];
        }
        // Growth-delta LUT: index by U>>6 (0..1023). Bakes in 1/T and u16 scale.
        for (int i = 0; i < 1024; i++) {
            float u = i / 1023.0f;
            float gz = 2.0f * bell_(u, g.mu, g.sigma) - 1.0f;
            growthLUT_[i] = (int32_t)lroundf((1.0f / g.T) * gz * 65535.0f);
        }
    }

    inline Acc macc(Acc acc, int t, State sample) const {
        return acc + (int64_t)tapW[t] * (int64_t)sample;
    }

    inline State apply(State before, Acc acc) const {
        int64_t U = acc >> 16;                 // back to u16 scale
        if (U < 0) U = 0; else if (U > 65535) U = 65535;
        int idx = (int)(U >> 6);               // 0..1023
        if (idx < 0) idx = 0; else if (idx > 1023) idx = 1023;
        int32_t v = (int32_t)before + growthLUT_[idx];
        if (v < 0) v = 0; else if (v > 65535) v = 65535;
        return (State)v;
    }

private:
    static inline float bell_(float x, float m, float s) {
        float d = (x - m) / s;
        return expf(-(d * d) * 0.5f);
    }
    int32_t growthLUT_[1024];
};

}  // namespace chimera
