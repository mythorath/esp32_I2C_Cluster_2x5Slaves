// lenia_fixed.h - Bank A "instinct" policy: fixed-point (ESP32-C3 / RISC-V),
// multi-channel. uint16 state; Q16 self + cross kernels; int64 convolution
// accumulators; 1024-entry growth LUTs (self = 2*bell-1, cross = presence bump);
// signed Q8 interaction weights per channel. Validated in port_check.py check 4.
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

    int    nSelfTaps = 0, nCrossTaps = 0;
    int8_t selfDy[MAX_TAPS], selfDx[MAX_TAPS];
    int8_t crossDy[MAX_TAPS], crossDx[MAX_TAPS];
    int32_t selfW[MAX_TAPS], crossW[MAX_TAPS];   // Q16 kernel weights

    void configure(const Genome& g) {
        nSelfTaps = buildKernel_(g.n_beta, g.beta, g.mu_k, g.sigma_k, selfDy, selfDx, selfW);
        float beta1[GENOME_MAX_BETA] = {1.0f, 0.0f, 0.0f};
        nCrossTaps = buildKernel_(1, beta1, g.mu_kc, g.sigma_kc, crossDy, crossDx, crossW);
        for (int i = 0; i < 1024; i++) {
            float u = i / 1023.0f;
            selfLUT_[i] = (int32_t)lroundf((1.0f / g.T) * (2.0f * bell_(u, g.mu, g.sigma) - 1.0f) * 65535.0f);
            crossLUT_[i] = (int32_t)lroundf((1.0f / g.T) * bell_(u, g.mu_c, g.sigma_c) * 65535.0f);
        }
        interactQ_[0] = (int32_t)lroundf(g.w_prey * 256.0f);   // species 0
        interactQ_[1] = (int32_t)lroundf(g.w_pred * 256.0f);   // species 1
    }

    inline Acc maccSelf(Acc acc, int t, State sample) const {
        return acc + (int64_t)selfW[t] * (int64_t)sample;
    }
    inline Acc maccCross(Acc acc, int t, State sample) const {
        return acc + (int64_t)crossW[t] * (int64_t)sample;
    }

    inline State apply(State before, Acc accSelf, Acc accCross, int ch) const {
        int64_t us = accSelf >> 16; if (us < 0) us = 0; else if (us > 65535) us = 65535;
        int64_t uc = accCross >> 16; if (uc < 0) uc = 0; else if (uc > 65535) uc = 65535;
        int idxS = (int)(us >> 6); if (idxS > 1023) idxS = 1023;
        int idxC = (int)(uc >> 6); if (idxC > 1023) idxC = 1023;
        int32_t cross = (int32_t)(((int64_t)interactQ_[ch] * (int64_t)crossLUT_[idxC]) >> 8);
        int32_t v = (int32_t)before + selfLUT_[idxS] + cross;
        if (v < 0) v = 0; else if (v > 65535) v = 65535;
        return (State)v;
    }

private:
    static inline float bell_(float x, float m, float s) {
        float d = (x - m) / s;
        return expf(-(d * d) * 0.5f);
    }
    // Build sparse Q16 kernel taps; returns tap count.
    static int buildKernel_(int n_beta, const float* beta, float mu_k, float sigma_k,
                            int8_t* dy, int8_t* dx, int32_t* w) {
        const int R = KERNEL_R;
        const int B = n_beta < 1 ? 1 : n_beta;
        float raw[MAX_TAPS];
        int8_t ty[MAX_TAPS], tx[MAX_TAPS];
        int n = 0; float total = 0.0f;
        for (int ky = -R; ky <= R; ky++)
            for (int kx = -R; kx <= R; kx++) {
                float dist = sqrtf((float)(ky * ky + kx * kx)) / R;
                if (dist >= 1.0f) continue;
                float rb = dist * B; int shell = (int)rb; if (shell >= B) shell = B - 1;
                float wv = beta[shell] * bell_(rb - shell, mu_k, sigma_k);
                if (wv <= 1e-6f) continue;
                raw[n] = wv; ty[n] = (int8_t)ky; tx[n] = (int8_t)kx; n++; total += wv;
            }
        if (total <= 0.0f) total = 1.0f;
        for (int i = 0; i < n; i++) { w[i] = (int32_t)lroundf((raw[i] / total) * 65536.0f); dy[i] = ty[i]; dx[i] = tx[i]; }
        return n;
    }
    int32_t selfLUT_[1024], crossLUT_[1024];
    int32_t interactQ_[NCH];
};

}  // namespace chimera
