// stitch.h - assemble per-node downsampled tiles into the full world field.
//
// Each node stages a 2x-downsampled tile of its strip (CMD_GET_TILE). The
// master pulls them (throttled, off the hot path) and lays them out into one
// downsampled field used by the visualizer and organism detector. Downsampled
// (64 x 200) keeps master RAM small and is plenty for blob detection + viz.
#pragma once

#include <stdint.h>
#include "protocol.h"
#include "topology.h"
#include "bus_manager.h"

namespace chimera {

static constexpr int DS_W = WORLD_W / 2;          // 64
static constexpr int DS_STRIP_H = STRIP_H / 2;    // 20
static constexpr int DS_H = DS_STRIP_H * N_STRIPS; // 200
static constexpr int DS_PLANE = DS_W * DS_H;      // one species plane
static constexpr int TILE_PLANE = DS_STRIP_H * DS_W;
static constexpr int TILE_BYTES = TILE_PLANE * NCH;  // node sends NCH planes

class Stitch {
public:
    // Pull tiles from all online nodes into the stitched field. Returns the
    // number of strips successfully updated.
    int collect(BusManager& bus);

    // Combined (max-of-species) plane - used by the organism detector + OLED.
    const uint8_t* field() const { return field_; }
    uint8_t at(int row, int col) const { return field_[row * DS_W + col]; }
    // Per-species planes (NCH x DS_PLANE), channel-major - streamed to Selis.
    const uint8_t* fieldMulti() const { return field2_; }
    int channels() const { return NCH; }
    int width() const { return DS_W; }
    int height() const { return DS_H; }

    // Strip ownership for a stitched row (for bank-aware coloring).
    static int stripOfRow(int row) { return row / DS_STRIP_H; }

private:
    uint8_t field_[DS_PLANE] = {0};          // combined (detect/display)
    uint8_t field2_[DS_PLANE * NCH] = {0};   // per-species (web)
    uint8_t tile_[TILE_BYTES];
};

}  // namespace chimera
