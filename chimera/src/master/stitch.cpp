#include "stitch.h"

namespace chimera {

int Stitch::collect(BusManager& bus) {
    int updated = 0;
    for (int i = 0; i < N_STRIPS; i++) {
        if (!bus.online(i)) continue;
        if (!bus.readBuffer(i, CMD_GET_TILE, 0, tile_, TILE_BYTES)) continue;
        int rowBase = i * DS_STRIP_H;
        for (int r = 0; r < DS_STRIP_H; r++) {
            for (int c = 0; c < DS_W; c++) {
                uint8_t mx = 0;
                for (int ch = 0; ch < NCH; ch++) {
                    uint8_t v = tile_[ch * TILE_PLANE + r * DS_W + c];
                    field2_[ch * DS_PLANE + (rowBase + r) * DS_W + c] = v;
                    if (v > mx) mx = v;
                }
                field_[(rowBase + r) * DS_W + c] = mx;   // combined for detect/OLED
            }
        }
        updated++;
    }
    return updated;
}

}  // namespace chimera
