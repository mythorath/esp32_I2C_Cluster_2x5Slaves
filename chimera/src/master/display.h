// display.h - ST7789 240x240 "field guide" for the master.
//
// Renders the stitched downsampled field (Bank A tinted green/cyan, Bank B
// magenta/amber so the seam + migrations are visible) plus a rotating field
// guide: world vitals and the newest named organism, its biome of origin, and
// age. Drawing is throttled and uses an address-window row push so it never
// starves the generation loop. Hardware: software SPI on the wired pins
// (CS27 DC26 RST4 MOSI19 SCLK18) - same as the archived mining dashboard.
#pragma once

#include <stdint.h>
#include "world_state.h"
#include "stitch.h"

namespace chimera {

class Display {
public:
    void begin();
    void renderField(const Stitch& stitch);          // push the field block
    void renderVitals(const WorldVitals& v, const Organism* newest);
    void boot(const char* msg);

private:
    uint16_t colorFor(uint8_t u, int bank);
    void chrome();

    bool chromeDrawn_ = false;
    // field render block geometry
    static constexpr int FX = 6, FY = 38, FPX = 2;   // 2 px per DS column -> 128 wide
};

}  // namespace chimera
