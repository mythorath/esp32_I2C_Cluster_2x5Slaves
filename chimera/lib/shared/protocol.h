// protocol.h - Chimera Lenia master<->node I2C protocol + world geometry.
//
// The master is I2C master on BOTH buses; nodes are pure I2C slaves and never
// talk peer-to-peer. ESP32 I2C slaves cannot clock-stretch, so the master must
// never read before a node has staged data. We use a SOFTWARE polled barrier:
// the master polls each node's status byte (CMD_GET_STATUS) until all report
// ST_READY, then reads halos/stats. The per-bank attention line is asserted
// during read bursts to keep node compute from racing the I2C ISR.
#pragma once

#include <stdint.h>
#include <string.h>
#include "crc.h"

namespace chimera {

// ---------------------------------------------------------------------------
// World geometry (v1). Keep in sync with tools/sim/lenia_ref.py LeniaParams.
// ---------------------------------------------------------------------------
static constexpr int   WORLD_W   = 128;   // columns (toroidal within each strip)
static constexpr int   STRIP_H   = 40;    // interior rows per node strip
static constexpr int   KERNEL_R  = 13;    // kernel radius = halo depth
static constexpr int   N_STRIPS  = 10;    // 5 Bank A (C3) + 5 Bank B (S3)
static constexpr int   WORLD_H   = STRIP_H * N_STRIPS;  // 400

static constexpr int   KERNEL_DIM = 2 * KERNEL_R + 1;   // 27
static constexpr int   BUF_H      = STRIP_H + 2 * KERNEL_R;  // interior + 2 halos
static constexpr int   HALO_ROWS  = KERNEL_R;
static constexpr int   HALO_BYTES = WORLD_W * HALO_ROWS;    // 1664 (uint8 quantized)

// ---------------------------------------------------------------------------
// I2C addressing. Bus 0 = S3 (Bank B), Bus 1 = C3 (Bank A) - matches wiring.
// ---------------------------------------------------------------------------
static constexpr uint8_t S3_ADDR_BASE = 0x08;  // Bus 0: 0x08..0x0C
static constexpr uint8_t C3_ADDR_BASE = 0x18;  // Bus 1: 0x18..0x1C
static constexpr int     NODES_PER_BANK = 5;

// ---------------------------------------------------------------------------
// Opcodes (master -> node command byte, written as first byte of a transfer).
// ---------------------------------------------------------------------------
enum Opcode : uint8_t {
    CMD_TICK        = 0x10,  // M->N: compute one generation (+ optional genome delta)
    CMD_SET_GENOME  = 0x11,  // M->N: full genome (migration / colonization)
    CMD_SEND_HALO   = 0x12,  // N->M: stage boundary rows for master to read (chunked)
    CMD_RECV_HALO   = 0x13,  // M->N: neighbor boundary rows (chunked)
    CMD_GET_STATS   = 0x14,  // N->M: vital signs (small, fixed-size)
    CMD_GET_TILE    = 0x15,  // N->M: full strip, downsampled uint8 (chunked, throttled)
    CMD_MIGRATE_OUT = 0x16,  // N->M: organism patch + local genome (seam handoff)
    CMD_MIGRATE_IN  = 0x17,  // M->N: transcoded patch to stamp into the strip
    CMD_SEED        = 0x18,  // M->N: reseed strip with a pattern id
    CMD_RESET       = 0x19,  // M->N: clear strip / reset to noise seed
    CMD_GET_STATUS  = 0x1A,  // N->M: single status byte (drives the polled barrier)
    CMD_SET_CURSOR  = 0x1B,  // M->N: select {opcode, chunk index} for next read
};

// ---------------------------------------------------------------------------
// Node status byte (returned by CMD_GET_STATUS, also first byte of stat frames).
// ---------------------------------------------------------------------------
enum NodeStatus : uint8_t {
    ST_BOOT      = 0x00,  // not yet initialized
    ST_READY     = 0x01,  // generation computed, halo + stats staged
    ST_COMPUTING = 0x02,  // mid-generation (master must wait)
    ST_ERROR     = 0x03,  // last transfer failed CRC / bad opcode
};

// Bank identity (also the StripStats.bank value). A = C3/fixed "instinct",
// B = S3/float "memory". Lives here (not topology.h) so nodes can use it
// without knowing the ring layout.
enum Bank : uint8_t { BANK_A = 0, BANK_B = 1 };

// Pattern ids for CMD_SEED.
enum SeedPattern : uint8_t {
    SEED_EMPTY   = 0x00,
    SEED_ORBIUM  = 0x01,
    SEED_NOISE   = 0x02,
};

// ---------------------------------------------------------------------------
// Chunked framing. Wire buffers are small; raise with Wire.setBufferSize().
// Each chunk on the wire is: [seq][len][payload(len)][crc8].
// ---------------------------------------------------------------------------
static constexpr int WIRE_BUFFER     = 256;  // request via Wire.setBufferSize(WIRE_BUFFER)
static constexpr int CHUNK_PAYLOAD   = 200;  // bytes of payload per chunk (< WIRE_BUFFER - overhead)
static constexpr int FRAME_OVERHEAD  = 3;    // seq + len + crc8
static constexpr int CHUNK_FRAME_MAX = CHUNK_PAYLOAD + FRAME_OVERHEAD;

// Number of chunks needed to move `total` bytes.
inline int chunkCount(int total) {
    return (total + CHUNK_PAYLOAD - 1) / CHUNK_PAYLOAD;
}

// Frame header offsets within a chunk buffer.
static constexpr int FRAME_SEQ = 0;
static constexpr int FRAME_LEN = 1;
static constexpr int FRAME_PAYLOAD = 2;  // crc8 trails the payload

// ---------------------------------------------------------------------------
// Vital-signs / stats struct (CMD_GET_STATS payload). Fixed-size, all little
// endian on both ISAs (ESP32 is LE). Values are in unit-normalized form so the
// master can combine fixed-point (C3) and float (S3) banks uniformly.
// ---------------------------------------------------------------------------
struct __attribute__((packed)) StripStats {
    uint8_t  status;     // NodeStatus
    uint8_t  bank;       // 0 = A (C3/fixed), 1 = B (S3/float)
    uint16_t gen_lo;     // low 16 bits of generation counter
    float    mass;       // sum(A) / (W*H_strip)  in [0,1]
    float    activity;   // fraction of cells changed > eps last gen
    float    entropy;    // normalized spatial entropy [0,1]
    float    com_y;      // center of mass row (interior coords) or NaN
    float    com_x;      // center of mass col or NaN
    float    fitness;    // node-local fitness estimate [0,..]
};

static constexpr int STATS_BYTES = sizeof(StripStats);

// ---------------------------------------------------------------------------
// Chunk frame helpers: [seq][len][payload(len)][crc8]. Returns total frame
// bytes written. `src` is the full buffer; chunk `seq` covers payload bytes
// [seq*CHUNK_PAYLOAD, ...). crc8 covers seq+len+payload.
// ---------------------------------------------------------------------------
inline int buildFrame(const uint8_t* src, int total, int seq, uint8_t* out) {
    int off = seq * CHUNK_PAYLOAD;
    int len = total - off;
    if (len < 0) len = 0;
    if (len > CHUNK_PAYLOAD) len = CHUNK_PAYLOAD;
    out[FRAME_SEQ] = (uint8_t)seq;
    out[FRAME_LEN] = (uint8_t)len;
    if (len > 0) memcpy(out + FRAME_PAYLOAD, src + off, len);
    out[FRAME_PAYLOAD + len] = crc8(out, FRAME_PAYLOAD + len);
    return FRAME_PAYLOAD + len + 1;
}

// Validate a received frame and copy its payload into dst at the right offset.
// Returns the payload length on success, or -1 on CRC/format failure.
inline int parseFrame(const uint8_t* frame, int frameLen, uint8_t* dst, int dstCap) {
    if (frameLen < FRAME_OVERHEAD) return -1;
    int len = frame[FRAME_LEN];
    if (FRAME_PAYLOAD + len + 1 > frameLen) return -1;
    uint8_t want = frame[FRAME_PAYLOAD + len];
    if (crc8(frame, FRAME_PAYLOAD + len) != want) return -1;
    int off = frame[FRAME_SEQ] * CHUNK_PAYLOAD;
    if (off + len > dstCap) return -1;
    if (len > 0) memcpy(dst + off, frame + FRAME_PAYLOAD, len);
    return len;
}

}  // namespace chimera
