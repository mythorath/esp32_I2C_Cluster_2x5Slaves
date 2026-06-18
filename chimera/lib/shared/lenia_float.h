// lenia_float.h - Bank B "memory" policy: float32 + FPU (ESP32-S3 / LX7),
// multi-channel. float state; self + cross sparse-tap kernels; standard Lenia
// self growth (2*bell-1) + a signed cross "presence" growth term per channel.
// Smoother/more deliberate than the fixed bank. Validated in
// tools/sim/multichannel_ref.py.
#pragma once

#include <stdint.h>
#include <math.h>
#include "protocol.h"
#include "genome.h"

namespace chimera {

class FloatPolicy {
public:
    using State = float;
    using Acc   = float;

    static constexpr int MAX_TAPS = KERNEL_DIM * KERNEL_DIM;  // 729

    static inline State zero()    { return 0.0f; }
    static inline Acc   zeroAcc() { return 0.0f; }
    static inline float toUnit(State s)  { return s; }
    static inline State fromUnit(float u) { return u; }

    int   nSelfTaps = 0, nCrossTaps = 0;
    int8_t selfDy[MAX_TAPS], selfDx[MAX_TAPS];
    int8_t crossDy[MAX_TAPS], crossDx[MAX_TAPS];
    float  selfW[MAX_TAPS], crossW[MAX_TAPS];

    void configure(const Genome& g) {
        mu_ = g.mu; sigma_ = g.sigma; invT_ = 1.0f / g.T;
        mu_c_ = g.mu_c; sigma_c_ = g.sigma_c;
        interactW_[0] = g.w_prey;
        interactW_[1] = g.w_pred;
        nSelfTaps = buildKernel_(g.n_beta, g.beta, g.mu_k, g.sigma_k, selfDy, selfDx, selfW);
        float beta1[GENOME_MAX_BETA] = {1.0f, 0.0f, 0.0f};
        nCrossTaps = buildKernel_(1, beta1, g.mu_kc, g.sigma_kc, crossDy, crossDx, crossW);
    }

    inline Acc maccSelf(Acc acc, int t, State sample) const { return acc + selfW[t] * sample; }
    inline Acc maccCross(Acc acc, int t, State sample) const { return acc + crossW[t] * sample; }

    inline State apply(State before, Acc accSelf, Acc accCross, int ch) const {
        float selfG = 2.0f * bell_(accSelf, mu_, sigma_) - 1.0f;       // [-1,1]
        float presence = bell_(accCross, mu_c_, sigma_c_);             // [0,1], 0 when absent
        float v = before + invT_ * (selfG + interactW_[ch] * presence);
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

private:
    static inline float bell_(float x, float m, float s) {
        float d = (x - m) / s;
        return expf(-(d * d) * 0.5f);
    }
    static int buildKernel_(int n_beta, const float* beta, float mu_k, float sigma_k,
                            int8_t* dy, int8_t* dx, float* w) {
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
        for (int i = 0; i < n; i++) { w[i] = raw[i] / total; dy[i] = ty[i]; dx[i] = tx[i]; }
        return n;
    }
    float mu_ = 0.15f, sigma_ = 0.015f, invT_ = 0.1f;
    float mu_c_ = 0.20f, sigma_c_ = 0.05f;
    float interactW_[NCH] = {0.0f, 0.0f};
};

}  // namespace chimera
