// detect.h - self-cataloguing: find, track, and name organisms (Phase 7b/5c).
//
// On the stitched (downsampled) field, run connected-component detection to find
// coherent structures, track them frame-to-frame by nearest centroid, and give
// each a generated name, birth time, trajectory, and death time. This "field
// guide" feeds the OLED/ST7789 + narrator: the system discovers and names its
// own creatures.
#pragma once

#include <stdint.h>
#include "world_state.h"
#include "stitch.h"
#include "lineage.h"
#include "led_events.h"
#include "telemetry.h"

namespace chimera {

class Detector {
public:
    void begin();   // allocate label/stack buffers on the heap (static DRAM is tight)
    void reset();   // forget all tracked organisms (full environment reset)
    void update(const Stitch& stitch, uint32_t gen, WorldVitals& vitals,
                Lineage* lineage, LedEvents* leds, TelemetryClient* web);

    int aliveCount() const;
    const Organism* newest() const;
    const Organism* organisms() const { return orgs_; }

private:
    int  labelComponents(const uint8_t* f, int w, int h);  // -> nComponents
    void nameOrganism(Organism& o);

    Organism orgs_[MAX_ORGANISMS];
    uint8_t* labels_ = nullptr;   // [DS_W*DS_H] visited mask (0/1), heap in begin()
    int16_t* stack_ = nullptr;    // [STACK_CAP] flood-fill frontier, heap in begin()

    // Flood-fill frontier never approaches the full field for this fragmented,
    // low-mass world (components are small); a bounded stack keeps ~21 KB of heap
    // free for WiFi/LwIP TCP buffers. Cells past the cap are marked visited but
    // not expanded - only matters for pathological full-field blobs (not organisms).
    static constexpr int STACK_CAP = 2048;

    // per-frame component scratch
    struct Comp { float x, y, mass; int area; };
    Comp comps_[MAX_ORGANISMS];

    uint16_t nextId_ = 1;
    int newestIdx_ = -1;
    uint32_t nameSeed_ = 0x51EED;

    // Tuned for the 2-species world: only bright, coherent cores count as
    // organisms. Too low/small and turbulent speckle saturates MAX_ORGANISMS,
    // flooding births/deaths every frame (WS write storm). These keep detections
    // to a handful of persistent structures.
    static constexpr uint8_t THRESH = 64;      // ~0.25 * 255 (bright cores only)
    static constexpr int MIN_AREA = 8;
    static constexpr int MAX_AREA = (DS_W * DS_STRIP_H * 2);  // ~2 strips
    static constexpr float MATCH_DIST = 9.0f;  // DS cells
    static constexpr int GRACE = 3;            // missed frames before death
    static constexpr int BIRTH_CONFIRM = 3;    // frames a structure must persist
                                               // before it's announced as a birth
                                               // (filters turbulent flicker -> no
                                               // event/WS storm, real orgs only)
};

}  // namespace chimera
