#include <Arduino.h>
#include "halo_router.h"

namespace chimera {

void HaloRouter::begin() {
    int fails = 0;
    for (int i = 0; i < N_STRIPS; i++) {
        haloTop_[i] = (uint8_t*)malloc(HALO_BYTES);
        haloBottom_[i] = (uint8_t*)malloc(HALO_BYTES);
        if (haloTop_[i]) memset(haloTop_[i], 0, HALO_BYTES);
        else fails++;
        if (haloBottom_[i]) memset(haloBottom_[i], 0, HALO_BYTES);
        else fails++;
    }
    if (fails) Serial.printf("[halo] WARNING: %d halo buffer(s) failed to alloc\n", fails);
}

bool HaloRouter::collect(BusManager& bus) {
    bool ok = true;
    for (int i = 0; i < N_STRIPS; i++) {
        if (!bus.online(i)) continue;
        if (!haloTop_[i] || !haloBottom_[i]) { ok = false; continue; }  // never DMA into null
        ok &= bus.readBuffer(i, CMD_SEND_HALO, 0, haloTop_[i], HALO_BYTES);
        ok &= bus.readBuffer(i, CMD_SEND_HALO, 1, haloBottom_[i], HALO_BYTES);
    }
    return ok;
}

// The seam between number-systems is a real transformation, not a copy:
//  - into MEMORY (Bank B): IMPRINT - reinforce the incoming pattern so it tends
//    to persist (memory "holds onto" what crosses in).
//  - into INSTINCT (Bank A): ENERGIZE - decay + dither so the pattern arrives
//    destabilized/reactive (instinct doesn't preserve, it reacts).
// Applies uniformly across both species planes (HALO_BYTES).
void HaloRouter::seamTransform(uint8_t* buf, bool isSeam, int toBank) {
    if (!isSeam) return;
    // coupling_ scales the seam gain so the two hemispheres can "breathe" between
    // a fully integrated world (1.0) and near-independent evolution (low).
    const float gain = ((toBank == BANK_B) ? 1.15f : 0.88f) * coupling_;
    const bool dither = dither_ && (toBank == BANK_A);
    for (int i = 0; i < HALO_BYTES; i++) {
        int v = (int)(buf[i] * gain + 0.5f);
        if (dither) {
            ditherState_ ^= ditherState_ << 13;
            ditherState_ ^= ditherState_ >> 17;
            ditherState_ ^= ditherState_ << 5;
            v += (int)(ditherState_ & 3) - 1;   // -1..+2 jitter -> reactivity
        }
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
        if (topSrc) {
            memcpy(recv_, topSrc, HALO_BYTES);
            bool seamTop = bus.online(above) && isSeamAbove(i);
            seamTransform(recv_, seamTop, stripBank(i));
            ok &= bus.writeBuffer(i, CMD_RECV_HALO, 0, recv_, HALO_BYTES);
            if (ev) { ev->exchanges++; if (seamTop) ev->seamCrossings++; }
        } else {
            ok = false;
        }

        // BOTTOM halo of i comes from the TOP edge of the strip below.
        const uint8_t* botSrc = bus.online(below) ? haloTop_[below] : haloTop_[i];
        if (botSrc) {
            memcpy(recv_, botSrc, HALO_BYTES);
            bool seamBot = bus.online(below) && isSeamBelow(i);
            seamTransform(recv_, seamBot, stripBank(i));
            ok &= bus.writeBuffer(i, CMD_RECV_HALO, 1, recv_, HALO_BYTES);
            if (ev) { ev->exchanges++; if (seamBot) ev->seamCrossings++; }
        } else {
            ok = false;
        }
    }
    return ok;
}

}  // namespace chimera
