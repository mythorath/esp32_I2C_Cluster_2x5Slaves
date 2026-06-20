// halo_router.h - collect strip boundary rows and route them to ring neighbors.
//
// Each generation: read every online node's top/bottom edge (uint8 quantized),
// then hand node i's TOP halo = neighborAbove's BOTTOM edge, and node i's BOTTOM
// halo = neighborBelow's TOP edge, wrapping around the 10-strip torus. Offline
// neighbors fall back to self-wrap so a partial ring (Phase 4: 2 nodes) still
// lives.
//
// Seam note: halos are already a common uint8 interchange format, so crossing
// the A<->B seam is identity at the halo level - the float<->fixed transcode
// happens natively inside each node's fromUnit(). Optional seam dithering
// reduces banding when a smooth float edge lands in the quantized bank.
#pragma once

#include <stdint.h>
#include "protocol.h"
#include "topology.h"
#include "bus_manager.h"

namespace chimera {

struct HaloEvents {
    uint32_t seamCrossings = 0;   // halo frames that crossed an A<->B seam
    uint32_t exchanges = 0;       // total halo frames routed this gen
};

class HaloRouter {
public:
    void begin();   // allocate halo buffers on the heap (keeps static DRAM small)
    void setDither(bool on) { dither_ = on; }

    // Inter-hemisphere coupling [0,1]: scales how strongly a pattern crossing the
    // A<->B seam influences the destination bank. The master "breathes" this over
    // time (1 = fully integrated world, low = the two halves evolve independently).
    void setCoupling(float c) { coupling_ = c < 0 ? 0 : (c > 1 ? 1 : c); }
    float coupling() const { return coupling_; }

    // Read all online nodes' staged edges into haloTop_/haloBottom_.
    bool collect(BusManager& bus);

    // Compute and write each online node's incoming halos for the next tick.
    bool route(BusManager& bus, HaloEvents* ev = nullptr);

    const uint8_t* topEdge(int strip) const { return haloTop_[strip]; }
    const uint8_t* bottomEdge(int strip) const { return haloBottom_[strip]; }

private:
    // Transform a halo crossing the A<->B seam: into MEMORY (Bank B) it is
    // imprinted/reinforced; into INSTINCT (Bank A) it is energized + dithered.
    void seamTransform(uint8_t* buf, bool isSeam, int toBank);

    // Heap-allocated (begin()) to avoid blowing the ESP32 static DRAM segment.
    uint8_t* haloTop_[N_STRIPS] = {nullptr};
    uint8_t* haloBottom_[N_STRIPS] = {nullptr};
    uint8_t recv_[HALO_BYTES];
    bool dither_ = true;
    float coupling_ = 1.0f;
    uint32_t ditherState_ = 0x1234567u;
};

}  // namespace chimera
