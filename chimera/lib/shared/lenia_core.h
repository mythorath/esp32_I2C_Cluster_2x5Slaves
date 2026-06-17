// lenia_core.h - shared, Policy-templated Lenia strip engine.
//
// One node owns one horizontal strip (WORLD_W x STRIP_H) of the torus. The
// buffer carries KERNEL_R halo rows above and below the interior so a single
// naive convolution covers every interior cell; X wraps toroidally within the
// strip (WORLD_W is a power of two -> mask wrap).
//
// All number-system specifics (uint16+LUT for Bank A, float32 for Bank B) live
// in a Policy (see lenia_fixed.h / lenia_float.h). The Policy precomputes the
// sparse nonzero kernel taps (the kernel is a thin annulus, so most of the
// 27x27 window is zero) and supplies the accumulate + growth/clip steps. The
// core handles geometry, halos, stats, seeding, and quantized halo I/O.
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
static constexpr int   TOTAL  = BUF_H * WORLD_W;   // cells per buffer (with halos)
static constexpr float ACT_EPS = 0.01f;            // activity threshold

template <class Policy>
class LeniaStrip {
public:
    using State = typename Policy::State;

    LeniaStrip() : cur_(0), gen_(0) {}

    // buf_[0]/buf_[1] must be provided by the node (heap or PSRAM) before use.
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
        for (int i = 0; i < TOTAL; i++) { buf_[0][i] = Policy::zero(); buf_[1][i] = Policy::zero(); }
    }

    State* current() { return buf_[cur_]; }
    State* next()    { return buf_[cur_ ^ 1]; }

    // Index helpers (buffer coords: row 0..BUF_H-1, col 0..W-1).
    static inline int idx(int row, int col) { return row * W + col; }
    // Interior cell (r in 0..IH-1, c in 0..W-1) maps to buffer row r+HALO.
    inline State interiorGet(int r, int c) const { return buf_[cur_][idx(r + HALO, c)]; }
    inline void  interiorSet(int r, int c, State v) { buf_[cur_][idx(r + HALO, c)] = v; }

    // -------- halo I/O (uint8 quantized, HALO_ROWS x W) --------
    // Master reads our top/bottom interior edges and hands them to neighbors.
    void extractTopHalo(uint8_t* out) const {     // our top HALO interior rows
        const State* b = buf_[cur_];
        for (int r = 0; r < HALO; r++)
            for (int c = 0; c < W; c++)
                out[r * W + c] = unitToByte(Policy::toUnit(b[idx(r + HALO, c)]));
    }
    void extractBottomHalo(uint8_t* out) const {  // our bottom HALO interior rows
        const State* b = buf_[cur_];
        for (int r = 0; r < HALO; r++)
            for (int c = 0; c < W; c++)
                out[r * W + c] = unitToByte(Policy::toUnit(b[idx(HALO + IH - HALO + r, c)]));
    }
    // Neighbor edges written into our halo regions before the next step.
    void injectTopHalo(const uint8_t* in) {       // rows [0,HALO)
        State* b = buf_[cur_];
        for (int r = 0; r < HALO; r++)
            for (int c = 0; c < W; c++)
                b[idx(r, c)] = Policy::fromUnit(byteToUnit(in[r * W + c]));
    }
    void injectBottomHalo(const uint8_t* in) {     // rows [HALO+IH, BUF_H)
        State* b = buf_[cur_];
        for (int r = 0; r < HALO; r++)
            for (int c = 0; c < W; c++)
                b[idx(HALO + IH + r, c)] = Policy::fromUnit(byteToUnit(in[r * W + c]));
    }

    // Standalone/bench mode: make this single strip a full vertical torus by
    // copying its own interior edges into its halo bands (no master needed).
    void selfWrapHalos() {
        State* b = buf_[cur_];
        for (int r = 0; r < HALO; r++)
            for (int c = 0; c < W; c++) {
                b[idx(r, c)] = b[idx(HALO + IH - HALO + r, c)];        // top halo <- bottom edge
                b[idx(HALO + IH + r, c)] = b[idx(HALO + r, c)];        // bottom halo <- top edge
            }
    }

    // -------- one generation --------
    void step() {
        const State* src = buf_[cur_];
        State* dst = buf_[cur_ ^ 1];
        const int nt = policy_.nTaps;
        lastActivity_ = 0;
        for (int r = HALO; r < HALO + IH; r++) {
            const int rowBase = r * W;
            for (int c = 0; c < W; c++) {
                typename Policy::Acc acc = Policy::zeroAcc();
                for (int t = 0; t < nt; t++) {
                    const int rr = r + policy_.tapDy[t];
                    const int cc = (c + policy_.tapDx[t]) & WMASK;
                    acc = policy_.macc(acc, t, src[rr * W + cc]);
                }
                State before = src[rowBase + c];
                State after = policy_.apply(before, acc);
                dst[rowBase + c] = after;
                if (fabsf(Policy::toUnit(after) - Policy::toUnit(before)) > ACT_EPS) lastActivity_++;
            }
        }
        // Copy interior edges within dst's halo bands are NOT set here; the
        // master re-injects neighbor halos each generation. Flip buffers.
        cur_ ^= 1;
        gen_++;
    }

    // -------- seeding --------
    void seed(uint8_t pattern, uint32_t seedVal = 0) {
        clear();
        switch (pattern) {
            case SEED_EMPTY: break;
            case SEED_ORBIUM: stampOrbium(); break;
            case SEED_NOISE: {
                Rng rng(seedVal ? seedVal : 0xA11FE);
                for (int r = 0; r < IH; r++)
                    for (int c = 0; c < W; c++)
                        interiorSet(r, c, Policy::fromUnit(rng.unit() * 0.6f));
                break;
            }
            default: break;
        }
    }

    void stampOrbium(int top = IH / 2 - 10, int left = W / 2 - 10) {
        for (int r = 0; r < 20; r++) {
            int rr = top + r;
            if (rr < 0 || rr >= IH) continue;
            for (int c = 0; c < 20; c++) {
                int cc = (left + c) & WMASK;
                interiorSet(rr, cc, Policy::fromUnit(ORBIUM[r][c]));
            }
        }
    }

    // -------- stats (unit-normalized, bank-agnostic) --------
    StripStats computeStats() const {
        StripStats s;
        s.status = ST_READY;
        s.bank = genome_.bank;
        s.gen_lo = (uint16_t)(gen_ & 0xFFFF);
        double mass = 0.0, sy = 0.0, sx = 0.0;
        int hist[16] = {0};
        const State* b = buf_[cur_];
        for (int r = 0; r < IH; r++) {
            for (int c = 0; c < W; c++) {
                float u = Policy::toUnit(b[idx(r + HALO, c)]);
                mass += u;
                sy += u * r;
                sx += u * c;
                int bin = (int)(u * 15.999f);
                if (bin < 0) bin = 0;
                if (bin > 15) bin = 15;
                hist[bin]++;
            }
        }
        int cells = IH * W;
        s.mass = (float)(mass / cells);
        s.activity = (float)lastActivity_ / cells;
        if (mass > 1e-6) { s.com_y = (float)(sy / mass); s.com_x = (float)(sx / mass); }
        else { s.com_y = NAN; s.com_x = NAN; }
        // normalized spatial entropy
        double h = 0.0;
        for (int i = 0; i < 16; i++) {
            if (!hist[i]) continue;
            double p = (double)hist[i] / cells;
            h -= p * log(p);
        }
        s.entropy = (float)(h / log(16.0));
        // node-local base fitness: structure (organism-sized mass) + activity.
        // Bank B augments this with PSRAM temporal persistence in node_s3.
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
