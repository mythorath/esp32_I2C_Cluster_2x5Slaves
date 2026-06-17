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
#include "webserver.h"

namespace chimera {

class Detector {
public:
    void begin();   // allocate label/stack buffers on the heap (static DRAM is tight)
    void update(const Stitch& stitch, uint32_t gen, WorldVitals& vitals,
                Lineage* lineage, LedEvents* leds, WebViz* web);

    int aliveCount() const;
    const Organism* newest() const;
    const Organism* organisms() const { return orgs_; }

private:
    int  labelComponents(const uint8_t* f, int w, int h);  // -> nComponents
    void nameOrganism(Organism& o);

    Organism orgs_[MAX_ORGANISMS];
    int16_t* labels_ = nullptr;   // [DS_W*DS_H], heap-allocated in begin()
    int16_t* stack_ = nullptr;    // [DS_W*DS_H], heap-allocated in begin()

    // per-frame component scratch
    struct Comp { float x, y, mass; int area; };
    Comp comps_[MAX_ORGANISMS];

    uint16_t nextId_ = 1;
    int newestIdx_ = -1;
    uint32_t nameSeed_ = 0x51EED;

    static constexpr uint8_t THRESH = 38;      // ~0.15 * 255
    static constexpr int MIN_AREA = 4;
    static constexpr int MAX_AREA = (DS_W * DS_STRIP_H * 2);  // ~2 strips
    static constexpr float MATCH_DIST = 9.0f;  // DS cells
    static constexpr int GRACE = 3;            // missed frames before death
};

}  // namespace chimera
