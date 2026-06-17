// lenia_float.h - Bank B "memory" policy: float32 + FPU (ESP32-S3 / LX7).
//
// State is float in [0,1]; the kernel is normalized so the convolution result
// is already a unit-scale potential U. Smoother and more deliberate than the
// fixed-point bank. The sparse-tap convolution skips the (large) zero region of
// the annular kernel. SIMD: the per-cell tap loop is the hot spot; an esp-dsp
// dot-product or LX7 EE.* intrinsics pass is a drop-in optimization later
// (kept scalar here for correctness-first bring-up).
#pragma once

#include <stdint.h>
#include <math.h>
#include "protocol.h"
#include "genome.h"

namespace chimera {

class FixedPolicy;  // fwd (for symmetry / docs)

class FloatPolicy {
public:
    using State = float;
    using Acc   = float;

    static constexpr int MAX_TAPS = KERNEL_DIM * KERNEL_DIM;  // 729

    static inline State zero()    { return 0.0f; }
    static inline Acc   zeroAcc() { return 0.0f; }
    static inline float toUnit(State s)  { return s; }
    static inline State fromUnit(float u) { return u; }

    int   nTaps = 0;
    int8_t tapDy[MAX_TAPS];
    int8_t tapDx[MAX_TAPS];
    float  tapW[MAX_TAPS];

    void configure(const Genome& g) {
        mu_ = g.mu; sigma_ = g.sigma; invT_ = 1.0f / g.T;
        const int R = KERNEL_R;
        const int B = g.n_beta < 1 ? 1 : g.n_beta;
        // First pass: compute raw weights and total for normalization.
        float total = 0.0f;
        float raw[MAX_TAPS];
        int8_t dy[MAX_TAPS], dx[MAX_TAPS];
        int n = 0;
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
        for (int i = 0; i < n; i++) { tapW[i] = raw[i] / total; tapDy[i] = dy[i]; tapDx[i] = dx[i]; }
    }

    inline Acc macc(Acc acc, int t, State sample) const { return acc + tapW[t] * sample; }

    inline State apply(State before, Acc acc) const {
        float gz = 2.0f * bell_(acc, mu_, sigma_) - 1.0f;   // growth in [-1,1]
        float v = before + invT_ * gz;
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

private:
    static inline float bell_(float x, float m, float s) {
        float d = (x - m) / s;
        return expf(-(d * d) * 0.5f);
    }
    float mu_ = 0.15f, sigma_ = 0.015f, invT_ = 0.1f;
};

}  // namespace chimera
