// host_test.cpp - compile the SHARED Lenia library on a desktop and confirm a
// strip stays alive + glides, without any ESP32 hardware. The shared lib has no
// Arduino dependencies, so this builds with a stock C++17 compiler:
//
//   g++ -std=c++17 -I../../lib/shared host_test.cpp -o host_test && ./host_test
//
// It exercises BOTH banks' number systems (FloatPolicy and FixedPolicy) through
// the same LeniaStrip<> core used on-device, including uint8 halo quantization.
#include <cstdio>
#include <cmath>
#include <vector>

#include "protocol.h"
#include "genome.h"
#include "lenia_core.h"
#include "lenia_float.h"
#include "lenia_fixed.h"

using namespace chimera;

template <class Policy>
bool run(const char* name, const Genome& g) {
    static typename Policy::State a[NCH * TOTAL], b[NCH * TOTAL];
    LeniaStrip<Policy> strip;
    strip.attach(a, b);
    strip.setGenome(g);
    strip.seed(SEED_ORBIUM);

    StripStats s0 = strip.computeStats();
    float m0 = s0.mass, cy0 = s0.com_y, cx0 = s0.com_x;

    // Round-trip halos through uint8 each step (matches the real master path).
    static uint8_t top[HALO_BYTES], bot[HALO_BYTES];
    int steps = 240;
    for (int i = 0; i < steps; i++) {
        strip.extractTopHalo(top);
        strip.extractBottomHalo(bot);
        // self-wrap via quantized halos: feed our own opposite edge back in
        strip.injectTopHalo(bot);
        strip.injectBottomHalo(top);
        strip.step();
    }
    StripStats s1 = strip.computeStats();
    float m1 = s1.mass;
    float dy = fmodf(s1.com_y - cy0 + STRIP_H / 2.0f + STRIP_H, (float)STRIP_H) - STRIP_H / 2.0f;
    float dx = fmodf(s1.com_x - cx0 + WORLD_W / 2.0f + WORLD_W, (float)WORLD_W) - WORLD_W / 2.0f;
    float drift = sqrtf(dy * dy + dx * dx);

    bool alive = m1 > 0.1f * m0 && m1 < 10.0f * m0;
    bool moved = drift > 2.0f;
    bool ok = alive && moved;
    printf("%-6s mass %.3f->%.3f drift=%.2f  %s\n",
           name, m0, m1, drift, ok ? "PASS" : "FAIL");
    return ok;
}

int main() {
    printf("=== Chimera Lenia shared-lib host test ===\n");
    bool ok = true;
    ok &= run<FloatPolicy>("float", defaultGenome(BANK_B));
    ok &= run<FixedPolicy>("fixed", defaultGenome(BANK_A));
    printf("RESULT: %s\n", ok ? "PASS - both banks live and glide" : "FAIL");
    return ok ? 0 : 1;
}
