#include <Arduino.h>
#include <math.h>
#include "detect.h"
#include "topology.h"

namespace chimera {

// Generated-name fragments. Bank A (instinct) gets terse, hard names; Bank B
// (memory) gets softer, flowing names - so a creature's biome shows in its name.
static const char* GENUS_A[] = {"Orb", "Scut", "Pyr", "Gyr", "Hel", "Kry", "Vex", "Tor"};
static const char* GENUS_B[] = {"Lumi", "Nimb", "Vela", "Aura", "Sela", "Mira", "Cael", "Noct"};
static const char* SPECIES[] = {"ium", "ata", "ova", "ix", "or", "yx", "ula", "een"};

void Detector::begin() {
    labels_ = (uint8_t*)malloc(DS_W * DS_H);              // 1 byte/cell visited mask
    stack_ = (int16_t*)malloc(STACK_CAP * sizeof(int16_t)); // bounded flood-fill frontier
    Serial.printf("[detect] begin labels=%p stack=%p freeHeap=%u largestBlock=%u\n",
                  (void*)labels_, (void*)stack_,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void Detector::reset() {
    for (int i = 0; i < MAX_ORGANISMS; i++) orgs_[i] = Organism();
    nextId_ = 1;
    newestIdx_ = -1;
}

void Detector::nameOrganism(Organism& o) {
    nameSeed_ ^= nameSeed_ << 13; nameSeed_ ^= nameSeed_ >> 17; nameSeed_ ^= nameSeed_ << 5;
    const char* genus = (o.bank == BANK_A) ? GENUS_A[(nameSeed_ >> 3) & 7]
                                           : GENUS_B[(nameSeed_ >> 3) & 7];
    const char* sp = SPECIES[(nameSeed_ >> 7) & 7];
    snprintf(o.name, sizeof(o.name), "%s%s-%u", genus, sp, (unsigned)(o.id));
}

int Detector::labelComponents(const uint8_t* f, int w, int h) {
    for (int i = 0; i < w * h; i++) labels_[i] = 0;   // 0 = unvisited
    int nComp = 0;
    for (int start = 0; start < w * h; start++) {
        if (f[start] < THRESH || labels_[start]) continue;
        // flood fill this component (4-connectivity, non-toroidal v1)
        int sp = 0;
        stack_[sp++] = start;
        labels_[start] = 1;
        double mass = 0, sx = 0, sy = 0;
        int area = 0;
        while (sp > 0) {
            int p = stack_[--sp];
            int r = p / w, c = p % w;
            float v = f[p] / 255.0f;
            mass += v; sx += v * c; sy += v * r; area++;
            const int nb[4] = {p - 1, p + 1, p - w, p + w};
            const bool ok[4] = {c > 0, c < w - 1, r > 0, r < h - 1};
            for (int k = 0; k < 4; k++) {
                if (!ok[k]) continue;
                int q = nb[k];
                // Mark visited regardless so a full stack can't re-seed the same
                // region as a phantom component; only expand if the stack has room.
                if (f[q] >= THRESH && !labels_[q]) { labels_[q] = 1; if (sp < STACK_CAP) stack_[sp++] = q; }
            }
        }
        if (area >= MIN_AREA && area <= MAX_AREA && nComp < MAX_ORGANISMS) {
            comps_[nComp].x = (float)(sx / mass);
            comps_[nComp].y = (float)(sy / mass);
            comps_[nComp].mass = (float)mass;
            comps_[nComp].area = area;
            nComp++;
        }
    }
    return nComp;
}

void Detector::update(const Stitch& stitch, uint32_t gen, WorldVitals& vitals,
                      Lineage* lineage, LedEvents* leds, TelemetryClient* web) {
    if (!labels_ || !stack_) return;   // begin() not called or alloc failed
    int nComp = labelComponents(stitch.field(), stitch.width(), stitch.height());

    bool compMatched[MAX_ORGANISMS] = {false};
    static int miss[MAX_ORGANISMS] = {0};

    // 1) match components to existing alive organisms by nearest centroid
    for (int oi = 0; oi < MAX_ORGANISMS; oi++) {
        if (!orgs_[oi].alive) continue;
        int best = -1; float bestD = MATCH_DIST;
        for (int ci = 0; ci < nComp; ci++) {
            if (compMatched[ci]) continue;
            float dx = comps_[ci].x - orgs_[oi].x;
            float dy = comps_[ci].y - orgs_[oi].y;
            float d = sqrtf(dx * dx + dy * dy);
            if (d < bestD) { bestD = d; best = ci; }
        }
        if (best >= 0) {
            compMatched[best] = true;
            orgs_[oi].vx = comps_[best].x - orgs_[oi].x;
            orgs_[oi].vy = comps_[best].y - orgs_[oi].y;
            orgs_[oi].x = comps_[best].x;
            orgs_[oi].y = comps_[best].y;
            orgs_[oi].mass = comps_[best].mass;
            orgs_[oi].fitness = comps_[best].mass * (1.0f + sqrtf(orgs_[oi].vx * orgs_[oi].vx +
                                                                  orgs_[oi].vy * orgs_[oi].vy));
            miss[oi] = 0;
            // Birth confirmation: a tracked structure only becomes a real, named
            // organism (and fires a fossil/LED/ticker event) once it has persisted
            // BIRTH_CONFIRM frames. Turbulent flicker never reaches this, so the
            // event stream stays a trickle instead of a per-frame storm.
            if (!orgs_[oi].announced && ++orgs_[oi].age >= BIRTH_CONFIRM) {
                orgs_[oi].announced = true;
                orgs_[oi].birthGen = gen;
                int strip = Stitch::stripOfRow((int)orgs_[oi].y);
                vitals.births++;
                if (lineage) lineage->record(LIN_BIRTH, gen, strip, strip, orgs_[oi].id,
                                             orgs_[oi].id, orgs_[oi].fitness);
                if (leds) leds->onBirth(orgs_[oi].bank, orgs_[oi].x, orgs_[oi].y);
                if (web) { char m[48]; snprintf(m, sizeof m, "%s born in Bank %c",
                                                orgs_[oi].name, orgs_[oi].bank == BANK_A ? 'A' : 'B');
                           web->pushEvent("birth", m); }
            }
        } else {
            if (++miss[oi] > GRACE) {
                orgs_[oi].alive = false;
                // Only announce a death for an organism the world ever heard about.
                if (orgs_[oi].announced) {
                    orgs_[oi].deathGen = gen;
                    vitals.deaths++;
                    if (lineage) lineage->record(LIN_DEATH, gen, -1, -1, 0, orgs_[oi].id, orgs_[oi].fitness);
                    if (leds) leds->onDeath(orgs_[oi].bank);
                    if (web) { char m[40]; snprintf(m, sizeof m, "%s vanished", orgs_[oi].name);
                               web->pushEvent("death", m); }
                }
            }
        }
    }

    // 2) unmatched components -> new organisms
    for (int ci = 0; ci < nComp; ci++) {
        if (compMatched[ci]) continue;
        int slot = -1;
        for (int oi = 0; oi < MAX_ORGANISMS; oi++) if (!orgs_[oi].alive) { slot = oi; break; }
        if (slot < 0) break;
        Organism& o = orgs_[slot];
        o.id = nextId_++;
        int strip = Stitch::stripOfRow((int)comps_[ci].y);
        o.bank = (strip < NODES_PER_BANK) ? BANK_A : BANK_B;
        o.alive = true;
        o.announced = false;   // provisional - must survive BIRTH_CONFIRM frames
        o.age = 1;
        o.birthGen = gen;
        o.deathGen = 0;
        o.x = comps_[ci].x; o.y = comps_[ci].y; o.vx = o.vy = 0;
        o.mass = comps_[ci].mass; o.fitness = comps_[ci].mass;
        nameOrganism(o);
        miss[slot] = 0;
        newestIdx_ = slot;
        // No birth event yet - that fires in the match path once it's confirmed.
    }
}

int Detector::aliveCount() const {
    // Report only confirmed organisms so the dashboard count reflects real,
    // persistent structures rather than transient flicker.
    int n = 0;
    for (int i = 0; i < MAX_ORGANISMS; i++) if (orgs_[i].alive && orgs_[i].announced) n++;
    return n;
}

const Organism* Detector::newest() const {
    const Organism* best = nullptr;
    uint32_t bg = 0;
    for (int i = 0; i < MAX_ORGANISMS; i++)
        if (orgs_[i].alive && orgs_[i].announced && orgs_[i].birthGen >= bg) {
            bg = orgs_[i].birthGen; best = &orgs_[i];
        }
    return best;
}

}  // namespace chimera
