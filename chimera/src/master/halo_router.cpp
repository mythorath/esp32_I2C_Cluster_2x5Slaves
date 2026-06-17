#include <Arduino.h>
#include "halo_router.h"

namespace chimera {

void HaloRouter::begin() {
    for (int i = 0; i < N_STRIPS; i++) {
        haloTop_[i] = (uint8_t*)malloc(HALO_BYTES);
        haloBottom_[i] = (uint8_t*)malloc(HALO_BYTES);
        if (haloTop_[i]) memset(haloTop_[i], 0, HALO_BYTES);
        if (haloBottom_[i]) memset(haloBottom_[i], 0, HALO_BYTES);
    }
}

bool HaloRouter::collect(BusManager& bus) {
    bool ok = true;
    for (int i = 0; i < N_STRIPS; i++) {
        if (!bus.online(i)) continue;
        ok &= bus.readBuffer(i, CMD_SEND_HALO, 0, haloTop_[i], HALO_BYTES);
        ok &= bus.readBuffer(i, CMD_SEND_HALO, 1, haloBottom_[i], HALO_BYTES);
    }
    return ok;
}

void HaloRouter::maybeDither(uint8_t* buf, bool isSeam) {
    if (!dither_ || !isSeam) return;
    // Tiny ordered-ish dither (+/-1 LSB) so a smooth float edge doesn't band
    // when it lands in the quantized bank. Cheap LFSR-driven.
    for (int i = 0; i < HALO_BYTES; i++) {
        ditherState_ ^= ditherState_ << 13;
        ditherState_ ^= ditherState_ >> 17;
        ditherState_ ^= ditherState_ << 5;
        int d = (int)(ditherState_ & 1) - (int)((ditherState_ >> 1) & 1);  // -1,0,+1-ish
        int v = (int)buf[i] + d;
        buf[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

bool HaloRouter::route(BusManager& bus, HaloEvents* ev) {
    bool ok = true;
    for (int i = 0; i < N_STRIPS; i++) {
        if (!bus.online(i)) continue;
        int above = neighborAbove(i);
        int below = neighborBelow(i);

        // TOP halo of i comes from the BOTTOM edge of the strip above.
        const uint8_t* topSrc = bus.online(above) ? haloBottom_[above] : haloBottom_[i];
        memcpy(recv_, topSrc, HALO_BYTES);
        bool seamTop = bus.online(above) && isSeamAbove(i);
        maybeDither(recv_, seamTop);
        ok &= bus.writeBuffer(i, CMD_RECV_HALO, 0, recv_, HALO_BYTES);
        if (ev) { ev->exchanges++; if (seamTop) ev->seamCrossings++; }

        // BOTTOM halo of i comes from the TOP edge of the strip below.
        const uint8_t* botSrc = bus.online(below) ? haloTop_[below] : haloTop_[i];
        memcpy(recv_, botSrc, HALO_BYTES);
        bool seamBot = bus.online(below) && isSeamBelow(i);
        maybeDither(recv_, seamBot);
        ok &= bus.writeBuffer(i, CMD_RECV_HALO, 1, recv_, HALO_BYTES);
        if (ev) { ev->exchanges++; if (seamBot) ev->seamCrossings++; }
    }
    return ok;
}

}  // namespace chimera
