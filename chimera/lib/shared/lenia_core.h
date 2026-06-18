// lenia_core.h - shared, Policy-templated MULTI-CHANNEL Lenia strip engine.
//
// One node owns one horizontal strip (WORLD_W x STRIP_H) of the torus, carrying
// NCH interacting species (channels). The buffer carries KERNEL_R halo rows
// above and below the interior so a single naive convolution covers every
// interior cell; X wraps toroidally within the strip (WORLD_W is a power of two
// -> mask wrap).
//
// Per channel each generation: a SELF convolution (own kernel) drives the
// standard Lenia growth, plus a CROSS convolution of the OTHER species through
// a sensing kernel drives a signed "presence" growth term (predator fed by
// prey, prey suppressed by predator). Validated in tools/sim/multichannel_ref.py
// and port_check.py (check 4).
//
// All number-system specifics (uint16+LUT for Bank A, float32 for Bank B) live
// in a Policy (lenia_fixed.h / lenia_float.h).
#pragma once

#include <stdint.h>
#include <math.h>
#include "protocol.h"
#include "genome.h"

namespace chimera {

// Canonical Orbium glider (20x20), validated in tools/sim/lenia_ref.py.
static const float ORBIUM[20][20] = {
    {0,0,0,0,0,0,0.1f,0.14f,0.1f,0,0,0.03f,0.03f,0,0,0.3f,0,0,0,0},
    {0,0,0,0,0,0.08f,0.24f,0.3f,0.3f,0.18f,0.14f,0.15f,0.16f,0.15f,0.09f,0.2f,0,0,0,0},
    {0,0,0,0,0,0.15f,0.34f,0.44f,0.46f,0.38f,0.18f,0.14f,0.11f,0.13f,0.19f,0.18f,0.45f,0,0,0},
    {0,0,0,0,0.06f,0.13f,0.39f,0.5f,0.5f,0.37f,0.06f,0,0,0,0.02f,0.16f,0.68f,0,0,0},
    {0,0,0,0.11f,0.17f,0.17f,0.33f,0.4f,0.38f,0.28f,0.14f,0,0,0,0,0,0.18f,0.42f,0,0},
    {0,0,0.09f,0.18f,0.13f,0.06f,0.08f,0.26f,0.32f,0.32f,0.27f,0,0,0,0,0,0,0.82f,0,0},
    {0.27f,0,0.16f,0.12f,0,0,0,0.25f,0.38f,0.44f,0.45f,0.34f,0,0,0,0,0,0.22f,0.17f,0},
    {0,0.07f,0.2f,0.02f,0,0,0,0.31f,0.48f,0.57f,0.6f,0.57f,0,0,0,0,0,0,0.49f,0},
    {0,0.59f,0.19f,0,0,0,0,0.2f,0.57f,0.69f,0.76f,0.76f,0.49f,0,0,0,0,0,0.36f,0},
    {0,0.58f,0.19f,0,0,0,0,0,0.67f,0.83f,0.9f,0.92f,0.87f,0.12f,0,0,0,0,0.22f,0.07f},
    {0,0,0.46f,0,0,0,0,0,0.7f,0.93f,1,1,1,0.61f,0,0,0,0,0.18f,0.11f},
    {0,0,0.82f,0,0,0,0,0,0.47f,1,1,0.98f,1,0.96f,0.27f,0,0,0,0.19f,0.1f},
    {0,0,0.46f,0,0,0,0,0,0.25f,1,1,0.84f,0.92f,0.97f,0.54f,0.14f,0.04f,0.1f,0.21f,0.05f},
    {0,0,0,0.4f,0,0,0,0,0.09f,0.8f,1,0.82f,0.8f,0.85f,0.63f,0.31f,0.18f,0.19f,0.2f,0.01f},
    {0,0,0,0.36f,0.1f,0,0,0,0.05f,0.54f,0.86f,0.79f,0.74f,0.72f,0.6f,0.39f,0.28f,0.24f,0.13f,0},
    {0,0,0,0.01f,0.3f,0.07f,0,0,0.08f,0.36f,0.64f,0.7f,0.64f,0.6f,0.51f,0.39f,0.29f,0.19f,0.04f,0},
    {0,0,0,0,0.1f,0.24f,0.14f,0.1f,0.15f,0.29f,0.45f,0.53f,0.52f,0.46f,0.4f,0.31f,0.21f,0.08f,0,0},
    {0,0,0,0,0,0.08f,0.21f,0.21f,0.22f,0.29f,0.36f,0.39f,0.37f,0.33f,0.26f,0.18f,0.09f,0,0,0},
    {0,0,0,0,0,0,0.03f,0.13f,0.19f,0.22f,0.24f,0.24f,0.23f,0.18f,0.13f,0.05f,0,0,0,0},
    {0,0,0,0,0,0,0,0,0.02f,0.06f,0.08f,0.09f,0.07f,0.05f,0.01f,0,0,0,0,0},
};

static constexpr int   W      = WORLD_W;
static constexpr int   WMASK  = WORLD_W - 1;       // requires WORLD_W power of two
static constexpr int   HALO   = KERNEL_R;
static constexpr int   IH     = STRIP_H;           // interior height
static constexpr int   TOTAL  = BUF_H * WORLD_W;   // cells per channel buffer (with halos)
static constexpr float ACT_EPS = 0.01f;            // activity threshold

// A buffer holds NCH channels back-to-back: channel c starts at c*TOTAL.
template <class Policy>
class LeniaStrip {
public:
    using State = typename Policy::State;

    LeniaStrip() : cur_(0), gen_(0) {}

    // bufA/bufB must each be NCH*TOTAL States (heap/PSRAM), provided by the node.
    void attach(State* bufA, State* bufB) {
        buf_[0] = bufA;
        buf_[1] = bufB;
        clear();
    }

    void setGenome(const Genome& g) {
        genome_ = g;
        policy_.configure(g);
    }
    const Genome& genome() const { return genome_; }

    void clear() {
        for (int i = 0; i < NCH * TOTAL; i++) { buf_[0][i] = Policy::zero(); buf_[1][i] = Policy::zero(); }
    }

    inline State* chan(int b, int ch) { return buf_[b] + ch * TOTAL; }
    inline const State* chan(int b, int ch) const { return buf_[b] + ch * TOTAL; }

    static inline int idx(int row, int col) { return row * W + col; }
    inline State interiorGet(int ch, int r, int c) const { return chan(cur_, ch)[idx(r + HALO, c)]; }
    inline void  interiorSet(int ch, int r, int c, State v) { chan(cur_, ch)[idx(r + HALO, c)] = v; }

    // -------- halo I/O: NCH planes stacked, [ch*HALO_PLANE + r*W + c] --------
    void extractTopHalo(uint8_t* out) const {
        for (int ch = 0; ch < NCH; ch++) {
            const State* b = chan(cur_, ch);
            uint8_t* o = out + ch * HALO_PLANE;
            for (int r = 0; r < HALO; r++)
                for (int c = 0; c < W; c++)
                    o[r * W + c] = unitToByte(Policy::toUnit(b[idx(r + HALO, c)]));
        }
    }
    void extractBottomHalo(uint8_t* out) const {
        for (int ch = 0; ch < NCH; ch++) {
            const State* b = chan(cur_, ch);
            uint8_t* o = out + ch * HALO_PLANE;
            for (int r = 0; r < HALO; r++)
                for (int c = 0; c < W; c++)
                    o[r * W + c] = unitToByte(Policy::toUnit(b[idx(HALO + IH - HALO + r, c)]));
        }
    }
    void injectTopHalo(const uint8_t* in) {
        for (int ch = 0; ch < NCH; ch++) {
            State* b = chan(cur_, ch);
            const uint8_t* i = in + ch * HALO_PLANE;
            for (int r = 0; r < HALO; r++)
                for (int c = 0; c < W; c++)
                    b[idx(r, c)] = Policy::fromUnit(byteToUnit(i[r * W + c]));
        }
    }
    void injectBottomHalo(const uint8_t* in) {
        for (int ch = 0; ch < NCH; ch++) {
            State* b = chan(cur_, ch);
            const uint8_t* i = in + ch * HALO_PLANE;
            for (int r = 0; r < HALO; r++)
                for (int c = 0; c < W; c++)
                    b[idx(HALO + IH + r, c)] = Policy::fromUnit(byteToUnit(i[r * W + c]));
        }
    }

    // Standalone/bench: make each channel a full vertical torus (no master).
    void selfWrapHalos() {
        for (int ch = 0; ch < NCH; ch++) {
            State* b = chan(cur_, ch);
            for (int r = 0; r < HALO; r++)
                for (int c = 0; c < W; c++) {
                    b[idx(r, c)] = b[idx(HALO + IH - HALO + r, c)];
                    b[idx(HALO + IH + r, c)] = b[idx(HALO + r, c)];
                }
        }
    }

    // -------- one generation (all channels) --------
    void step() {
        using Acc = typename Policy::Acc;
        const int ns = policy_.nSelfTaps;
        const int ncr = policy_.nCrossTaps;
        lastActivity_ = 0;
        for (int ch = 0; ch < NCH; ch++) {
            const State* self = chan(cur_, ch);
            const State* other = chan(cur_, (ch + 1) % NCH);   // NCH=2: the other species
            State* dst = chan(cur_ ^ 1, ch);
            for (int r = HALO; r < HALO + IH; r++) {
                const int rowBase = r * W;
                for (int c = 0; c < W; c++) {
                    Acc accSelf = Policy::zeroAcc();
                    for (int t = 0; t < ns; t++) {
                        const int rr = r + policy_.selfDy[t];
                        const int cc = (c + policy_.selfDx[t]) & WMASK;
                        accSelf = policy_.maccSelf(accSelf, t, self[rr * W + cc]);
                    }
                    Acc accCross = Policy::zeroAcc();
                    for (int t = 0; t < ncr; t++) {
                        const int rr = r + policy_.crossDy[t];
                        const int cc = (c + policy_.crossDx[t]) & WMASK;
                        accCross = policy_.maccCross(accCross, t, other[rr * W + cc]);
                    }
                    State before = self[rowBase + c];
                    State after = policy_.apply(before, accSelf, accCross, ch);
                    dst[rowBase + c] = after;
                    if (fabsf(Policy::toUnit(after) - Policy::toUnit(before)) > ACT_EPS) lastActivity_++;
                }
            }
        }
        cur_ ^= 1;
        gen_++;
    }

    // Memory bank only: blend the just-replaced previous field back into the
    // current one (temporal low-pass) so structures persist/ghost/trail. This is
    // what makes Bank B "remember" and look calm/enduring vs the instinct bank's
    // sharp, reactive organisms. Call right after step(). echo in [0,1).
    void blendPrevious(float echo) {
        if (echo <= 0.0f) return;
        const float keep = 1.0f - echo;
        for (int ch = 0; ch < NCH; ch++) {
            State* curB = chan(cur_, ch);
            const State* prevB = chan(cur_ ^ 1, ch);
            for (int r = HALO; r < HALO + IH; r++) {
                const int base = r * W;
                for (int c = 0; c < W; c++) {
                    float v = Policy::toUnit(curB[base + c]) * keep + Policy::toUnit(prevB[base + c]) * echo;
                    curB[base + c] = Policy::fromUnit(v);
                }
            }
        }
    }

    // -------- seeding --------
    void seed(uint8_t pattern, uint32_t seedVal = 0) {
        clear();
        switch (pattern) {
            case SEED_EMPTY: break;
            case SEED_ORBIUM:
                // two species seeded overlapping so they interact from gen 0
                stampOrbium(0, IH / 2 - 12, W / 2 - 4);
                stampOrbium(1, IH / 2 - 4,  W / 2 + 4);
                break;
            case SEED_NOISE: {
                Rng rng(seedVal ? seedVal : 0xA11FE);
                for (int ch = 0; ch < NCH; ch++)
                    for (int r = 0; r < IH; r++)
                        for (int c = 0; c < W; c++)
                            interiorSet(ch, r, c, Policy::fromUnit(rng.unit() * 0.6f));
                break;
            }
            default: break;
        }
    }

    void stampOrbium(int ch, int top, int left) {
        for (int r = 0; r < 20; r++) {
            int rr = top + r;
            if (rr < 0 || rr >= IH) continue;
            for (int c = 0; c < 20; c++) {
                int cc = (left + c) & WMASK;
                interiorSet(ch, rr, cc, Policy::fromUnit(ORBIUM[r][c]));
            }
        }
    }

    // -------- stats (combined over species + species-1 split) --------
    StripStats computeStats() const {
        StripStats s;
        s.status = ST_READY;
        s.bank = genome_.bank;
        s.gen_lo = (uint16_t)(gen_ & 0xFFFF);
        double mass = 0.0, mass1 = 0.0, sy = 0.0, sx = 0.0;
        int hist[16] = {0};
        for (int ch = 0; ch < NCH; ch++) {
            const State* b = chan(cur_, ch);
            for (int r = 0; r < IH; r++) {
                for (int c = 0; c < W; c++) {
                    float u = Policy::toUnit(b[idx(r + HALO, c)]);
                    mass += u;
                    if (ch == 1) mass1 += u;
                    sy += u * r;
                    sx += u * c;
                    int bin = (int)(u * 15.999f);
                    if (bin < 0) bin = 0;
                    if (bin > 15) bin = 15;
                    hist[bin]++;
                }
            }
        }
        int cells = IH * W;
        s.mass = (float)(mass / cells);
        s.mass1 = (float)(mass1 / cells);
        s.activity = (float)lastActivity_ / (cells * NCH);
        if (mass > 1e-6) { s.com_y = (float)(sy / mass); s.com_x = (float)(sx / mass); }
        else { s.com_y = NAN; s.com_x = NAN; }
        double h = 0.0;
        int htot = cells * NCH;
        for (int i = 0; i < 16; i++) {
            if (!hist[i]) continue;
            double p = (double)hist[i] / htot;
            h -= p * log(p);
        }
        s.entropy = (float)(h / log(16.0));
        float occTarget = 0.12f;
        float structure = expf(-(((s.mass - occTarget) / 0.1f) * ((s.mass - occTarget) / 0.1f)) / 2.0f);
        s.fitness = 1.0f * structure + 0.5f * s.activity;
        return s;
    }

    uint32_t generation() const { return gen_; }
    uint32_t lastActivity() const { return lastActivity_; }

private:
    static inline uint8_t unitToByte(float u) {
        int v = (int)(u * 255.0f + 0.5f);
        return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    static inline float byteToUnit(uint8_t b) { return b * (1.0f / 255.0f); }

    Policy policy_;
    Genome genome_;
    State* buf_[2] = {nullptr, nullptr};
    int cur_;
    uint32_t gen_;
    uint32_t lastActivity_ = 0;
};

}  // namespace chimera
