// world_state.h - master-side aggregate state shared by display/web/narrator.
#pragma once

#include <stdint.h>
#include "protocol.h"

namespace chimera {

static constexpr int MAX_ORGANISMS = 24;

struct Organism {
    uint16_t id = 0;
    char     name[16] = {0};
    uint8_t  bank = 0;            // origin bank
    bool     alive = false;
    uint32_t birthGen = 0;
    uint32_t deathGen = 0;
    float    x = 0, y = 0;        // stitched (downsampled) coords
    float    vx = 0, vy = 0;      // drift
    float    mass = 0;
    float    fitness = 0;
};

struct WorldVitals {
    uint32_t generation = 0;
    int      online = 0;
    float    worldMass = 0;       // mean strip mass
    float    worldActivity = 0;
    float    worldEntropy = 0;
    float    bestFitness = 0;
    int      bestStrip = -1;
    float    coupling = 1.0f;     // inter-hemisphere seam coupling [0,1]
    uint32_t organismsAlive = 0;
    uint32_t births = 0, deaths = 0, migrations = 0, seamCrossings = 0;

    // connectivity / network (for the display)
    bool     wifiOk = false;
    char     ip[20] = {0};        // master IP, or "headless"
    bool     nodeOnline[N_STRIPS] = {false};
};

}  // namespace chimera
