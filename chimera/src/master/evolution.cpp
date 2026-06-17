#include <Arduino.h>
#include <esp_system.h>
#include "evolution.h"
#include "topology.h"

namespace chimera {

void Evolution::begin(Genome*) {
    rng_ = Rng(esp_random() | 1u);
}

void Evolution::update(BusManager& bus, const StripStats* stats, Genome* genomes,
                       WorldVitals& vitals, uint32_t gen, LedEvents* leds, Lineage* lineage) {
    if (gen - lastHomeo_ >= HOMEO_EVERY) {
        lastHomeo_ = gen;
        homeostasis(bus, genomes, vitals, gen);
    }
    if (gen - lastIsland_ >= ISLAND_EVERY) {
        lastIsland_ = gen;
        islandStep(bus, stats, genomes, vitals, gen, leds, lineage);
    }
}

// Gentle global controller: keep the world in an "alive & interesting" band.
// Deltas are applied to every strip's genome so per-strip divergence (from
// island evolution) is preserved while the whole world is nudged together.
void Evolution::homeostasis(BusManager& bus, Genome* genomes, const WorldVitals& v, uint32_t gen) {
    // The band itself is the deadband: do nothing while in band. Only the safe
    // levers (sigma, dt=1/T) are driven here - mu belongs to the slow island GA,
    // and driving it from the fast loop can ratchet a saturating world to death.
    float dSigma = 0, dT = 0;
    bool act = false;
    if (v.worldMass < MASS_LO && v.worldActivity < ACT_LO) {  // truly dying/frozen
        dSigma = +D_SIGMA; dT = -D_T; act = true;             // grow more, speed up
    } else if (v.worldMass > MASS_HI) {                       // saturating -> calm down
        dSigma = -D_SIGMA; dT = +D_T; act = true;             // narrow growth, slow down
    }
    if (!act) return;
    for (int i = 0; i < N_STRIPS; i++) {
        genomes[i].sigma += dSigma;
        genomes[i].T += dT;
        clampGenome(genomes[i]);
        if (bus.online(i)) bus.setGenome(i, genomes[i]);
    }
    Serial.printf("  [homeo gen %lu] mass=%.3f act=%.3f -> dSig=%.4f dT=%.2f\n",
                  (unsigned long)gen, v.worldMass, v.worldActivity, dSigma, dT);
}

// Best strip index within a bank [base, base+NODES_PER_BANK); also returns worst.
int Evolution::bankBest(const StripStats* stats, int base, int& worstOut) const {
    int best = base, worst = base;
    float bf = -1e9f, wf = 1e9f;
    for (int i = base; i < base + NODES_PER_BANK; i++) {
        float f = stats[i].fitness;
        if (f > bf) { bf = f; best = i; }
        if (f < wf) { wf = f; worst = i; }
    }
    worstOut = worst;
    return best;
}

void Evolution::islandStep(BusManager& bus, const StripStats* stats, Genome* genomes,
                           WorldVitals& vitals, uint32_t gen, LedEvents* leds, Lineage* lineage) {
    // --- within-bank colonization: best genome replaces worst ---
    for (int bank = 0; bank < 2; bank++) {
        int base = bank * NODES_PER_BANK;
        int worst;
        int best = bankBest(stats, base, worst);
        if (best == worst) continue;
        float rate = (bank == BANK_A) ? RATE_A : RATE_B;

        if (rng_.unit() < WILDCARD_P) {
            // wildcard: fresh noise + mutated default keeps diversity alive
            genomes[worst] = defaultGenome((uint8_t)bank);
            genomes[worst].lineage_id = 1000 + gen % 1000;
            mutateGenome(genomes[worst], rng_, rate * 1.5f);
            if (bus.online(worst)) { bus.setGenome(worst, genomes[worst]); bus.seed(worst, SEED_NOISE, esp_random()); }
            if (lineage) lineage->record(LIN_WILDCARD, gen, -1, worst, genomes[worst].lineage_id, 0, stats[worst].fitness);
        } else {
            genomes[worst] = genomes[best];                 // colonize
            mutateGenome(genomes[worst], rng_, rate);        // diverge a bit
            genomes[worst].bank = (uint8_t)bank;
            if (bus.online(worst)) {
                bus.setGenome(worst, genomes[worst]);
                bus.seed(worst, SEED_ORBIUM, 0);             // transplant a seed organism
            }
            if (lineage) lineage->record(LIN_COLONIZE, gen, best, worst,
                                         genomes[best].lineage_id, 0, stats[best].fitness);
        }
    }

    // --- cross-seam migration: ferry a winner to the other bank ---
    if (gen - lastMigrate_ >= MIGRATE_EVERY) {
        lastMigrate_ = gen;
        int wA, wB;
        int bestA = bankBest(stats, 0, wA);
        int bestB = bankBest(stats, NODES_PER_BANK, wB);
        // send the globally stronger bank's winner into the other's weakest
        bool aStronger = stats[bestA].fitness >= stats[bestB].fitness;
        int src = aStronger ? bestA : bestB;
        int dst = aStronger ? wB : wA;
        uint8_t dstBank = stripBank(dst);
        genomes[dst] = genomes[src];          // transcode is native (bank-tagged)
        genomes[dst].bank = dstBank;
        mutateGenome(genomes[dst], rng_, (dstBank == BANK_A) ? RATE_A : RATE_B);
        if (bus.online(dst)) { bus.setGenome(dst, genomes[dst]); bus.seed(dst, SEED_ORBIUM, 0); }
        vitals.migrations++;
        if (leds) leds->onMigration(src, dst);
        if (lineage) lineage->record(LIN_MIGRATION, gen, src, dst,
                                     genomes[src].lineage_id, 0, stats[src].fitness);
        Serial.printf("  [migrate gen %lu] strip %d (bank %c) -> strip %d (bank %c)\n",
                      (unsigned long)gen, src, stripBank(src) == BANK_A ? 'A' : 'B',
                      dst, dstBank == BANK_A ? 'A' : 'B');
    }
}

}  // namespace chimera
