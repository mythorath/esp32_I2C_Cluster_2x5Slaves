// evolution.h - the autonomy core: homeostasis + island-model rule evolution.
//
// 7a Homeostasis (fast): watch world vital signs and nudge global parameters
//    (mu, sigma, dt=1/T) toward the edge of chaos so the world fights for its
//    own survival - dying -> widen/quicken, saturating -> narrow/calm.
//
// 7c Island evolution (slow): each strip carries a genome; strips compete on
//    fitness. Within a bank, the best genome colonizes the worst (copy+mutate+
//    reseed). Across the seam, the master ferries a winning genome to the other
//    bank at a controlled rate. Per-bank mutation rates + a wildcard reseed
//    floor keep the two islands on divergent, non-collapsing trajectories.
#pragma once

#include <stdint.h>
#include "genome.h"
#include "world_state.h"
#include "bus_manager.h"
#include "lineage.h"
#include "led_events.h"

namespace chimera {

class Evolution {
public:
    void begin(Genome* genomes);
    void update(BusManager& bus, const StripStats* stats, Genome* genomes,
                WorldVitals& vitals, uint32_t gen, LedEvents* leds, Lineage* lineage);

private:
    void homeostasis(BusManager& bus, Genome* genomes, const WorldVitals& v, uint32_t gen);
    void islandStep(BusManager& bus, const StripStats* stats, Genome* genomes,
                    WorldVitals& vitals, uint32_t gen, LedEvents* leds, Lineage* lineage);
    int  bankBest(const StripStats* stats, int base, int& worstOut) const;

    Rng rng_{0x9E3779B9u};
    uint32_t lastHomeo_ = 0, lastIsland_ = 0, lastMigrate_ = 0;

    // cadences (generations)
    static constexpr uint32_t HOMEO_EVERY = 20;
    static constexpr uint32_t ISLAND_EVERY = 300;
    static constexpr uint32_t MIGRATE_EVERY = 600;

    // homeostasis target bands + gains. NOTE: a moving glider is a low-mass but
    // HIGH-activity world - it's interesting, not dying - so "dying" is gated on
    // BOTH low mass AND low activity. Homeostasis maintains a living world in
    // band; resurrecting an empty one is evolution's wildcard-reseed job.
    static constexpr float MASS_LO = 0.010f, MASS_HI = 0.220f;
    static constexpr float ACT_LO = 0.005f;
    static constexpr float D_SIGMA = 0.0008f, D_MU = 0.003f, D_T = 0.3f;

    // per-bank mutation rates (A=instinct/volatile, B=memory/conservative)
    static constexpr float RATE_A = 0.6f, RATE_B = 0.3f;
    static constexpr float WILDCARD_P = 0.04f;   // chance worst gets fresh noise
};

}  // namespace chimera
