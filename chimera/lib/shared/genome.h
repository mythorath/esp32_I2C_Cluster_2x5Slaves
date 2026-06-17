// genome.h - per-strip Lenia genome (the unit of evolution, sec 5b/8).
//
// A genome fully specifies a strip's physics. Tiles compete on fitness; winners
// colonize neighbors and migrate across the seam. float32 everywhere (LX6/LX7
// emulate double). Serializes to a fixed-size little-endian byte block so it
// rides the I2C protocol unchanged on both banks.
#pragma once

#include <stdint.h>
#include <string.h>
#include <math.h>

namespace chimera {

static constexpr int GENOME_MAX_BETA = 3;  // up to 3 kernel shells

struct __attribute__((packed)) Genome {
    uint8_t  R;                       // kernel radius (cells); v1 fixed at KERNEL_R
    uint8_t  n_beta;                  // number of active kernel shells (1..3)
    uint8_t  bank;                    // 0 = A (C3/fixed), 1 = B (S3/float)
    uint8_t  _pad;
    float    T;                       // time resolution; dt = 1/T
    float    mu;                      // growth center
    float    sigma;                   // growth width
    float    mu_k;                    // kernel shell center (fraction of R)
    float    sigma_k;                 // kernel shell width
    float    beta[GENOME_MAX_BETA];   // per-shell peak heights
    uint32_t lineage_id;             // root ancestor id (fossil record)
    uint16_t generation;             // genome generations since lineage root
    uint16_t _pad2;
};

static constexpr int GENOME_BYTES = sizeof(Genome);

// Locked Orbium-class starting genome (validated in tools/sim/lenia_ref.py).
inline Genome defaultGenome(uint8_t bank) {
    Genome g;
    memset(&g, 0, sizeof(g));
    g.R = 13;
    g.n_beta = 1;
    g.bank = bank;
    g.T = 10.0f;
    g.mu = 0.15f;
    g.sigma = 0.015f;
    g.mu_k = 0.5f;
    g.sigma_k = 0.15f;
    g.beta[0] = 1.0f;
    g.beta[1] = 0.0f;
    g.beta[2] = 0.0f;
    g.lineage_id = 0;
    g.generation = 0;
    return g;
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Clamp every gene to a safe, life-supporting range so mutation/colonization
// can't push a strip into guaranteed death or NaN.
inline void clampGenome(Genome& g) {
    g.T       = clampf(g.T, 2.0f, 40.0f);
    g.mu      = clampf(g.mu, 0.05f, 0.40f);
    g.sigma   = clampf(g.sigma, 0.008f, 0.10f);   // floor keeps growth life-supporting
    g.mu_k    = clampf(g.mu_k, 0.15f, 0.85f);
    g.sigma_k = clampf(g.sigma_k, 0.04f, 0.30f);
    for (int i = 0; i < GENOME_MAX_BETA; i++) g.beta[i] = clampf(g.beta[i], 0.0f, 1.0f);
    if (g.n_beta < 1) g.n_beta = 1;
    if (g.n_beta > GENOME_MAX_BETA) g.n_beta = GENOME_MAX_BETA;
}

inline void serializeGenome(const Genome& g, uint8_t* out) {
    memcpy(out, &g, GENOME_BYTES);
}

inline Genome deserializeGenome(const uint8_t* in) {
    Genome g;
    memcpy(&g, in, GENOME_BYTES);
    return g;
}

// Tiny xorshift RNG so master and nodes can mutate without <random> overhead.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0xC0FFEEu) {}
    inline uint32_t next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s;
    }
    inline float unit() { return (next() >> 8) * (1.0f / 16777216.0f); }  // [0,1)
    inline float normal() {  // cheap approx N(0,1) via sum of uniforms
        float a = unit() + unit() + unit() + unit() + unit() + unit()
                + unit() + unit() + unit() + unit() + unit() + unit();
        return a - 6.0f;
    }
};

// Mutate genes by gaussian noise scaled by `rate`. `floor` keeps a minimum
// mutation so evolution never fully collapses diversity (sec 11). Per-bank rate
// differences keep the two islands on divergent trajectories.
inline void mutateGenome(Genome& g, Rng& rng, float rate, float floorRate = 0.01f) {
    float r = rate < floorRate ? floorRate : rate;
    g.mu      += rng.normal() * 0.02f  * r;
    g.sigma   += rng.normal() * 0.004f * r;
    g.T       += rng.normal() * 1.5f   * r;
    g.mu_k    += rng.normal() * 0.03f  * r;
    g.sigma_k += rng.normal() * 0.02f  * r;
    for (int i = 0; i < g.n_beta; i++) g.beta[i] += rng.normal() * 0.05f * r;
    g.generation++;
    clampGenome(g);
}

}  // namespace chimera
