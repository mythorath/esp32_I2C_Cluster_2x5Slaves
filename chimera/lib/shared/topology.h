// topology.h - the 10-strip torus ring, neighbor + seam tables.
//
// The world is a torus partitioned into 10 horizontal strips, one per node.
// Ring order around the torus (wrapping back to the top):
//
//     strip:   0   1   2   3   4 | 5   6   7   8   9 | (wrap to 0)
//     bank :   A   A   A   A   A | B   B   B   B   B
//     chip :  C3  C3  C3  C3  C3 |S3  S3  S3  S3  S3
//     bus  :   1   1   1   1   1 | 0   0   0   0   0
//
// Each strip has exactly two neighbors (above = i-1, below = i+1, mod 10),
// halving halo traffic vs 2D tiling. There are TWO seams where Bank A meets
// Bank B: edge 4<->5 and edge 9<->0. Both are bridged + transcoded (uint8<->
// float with dithering) by the master.
#pragma once

#include <stdint.h>
#include "protocol.h"

namespace chimera {

// Bank enum lives in protocol.h (A = C3/fixed, B = S3/float).

inline Bank stripBank(int i)  { return (i < NODES_PER_BANK) ? BANK_A : BANK_B; }
inline uint8_t stripBus(int i) { return (stripBank(i) == BANK_A) ? 1 : 0; }     // C3=bus1, S3=bus0

inline uint8_t stripAddr(int i) {
    return (stripBank(i) == BANK_A)
        ? (uint8_t)(C3_ADDR_BASE + i)                       // strips 0..4 -> 0x18..0x1C
        : (uint8_t)(S3_ADDR_BASE + (i - NODES_PER_BANK));   // strips 5..9 -> 0x08..0x0C
}

inline int neighborAbove(int i) { return (i - 1 + N_STRIPS) % N_STRIPS; }
inline int neighborBelow(int i) { return (i + 1) % N_STRIPS; }

// A ring edge is a seam when the two strips belong to different banks.
inline bool edgeIsSeam(int a, int b) { return stripBank(a) != stripBank(b); }
inline bool isSeamAbove(int i) { return edgeIsSeam(i, neighborAbove(i)); }
inline bool isSeamBelow(int i) { return edgeIsSeam(i, neighborBelow(i)); }

// Linear strip index for the master's stitched full field (row offset).
inline int stripRowOffset(int i) { return i * STRIP_H; }

}  // namespace chimera
