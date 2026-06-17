// crc.h - CRC-8 for chunked I2C frame integrity (shared by master + nodes).
//
// All multi-byte I2C transfers are framed as [seq][len][payload...][crc8] and
// the CRC is checked on receive; a mismatch triggers a retransmit of that frame
// (I2C buffers are small and the bus is noisy enough that this matters).
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace chimera {

// CRC-8 (poly 0x07, init 0x00) - small, table-free, adequate for short frames.
inline uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

}  // namespace chimera
