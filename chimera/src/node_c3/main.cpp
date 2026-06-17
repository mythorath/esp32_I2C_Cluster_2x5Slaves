// node_c3/main.cpp - Bank A "instinct" node (ESP32-C3 SuperMini, fixed-point).
//
// One node owns one horizontal strip. Pure I2C slave on bus 1 (master drives
// both buses). uint16 state, Q16 kernel, LUT growth - blockier, fast, no FPU
// pressure, no temporal history (persistence is Bank B's specialty; this bank
// is reflexive "instinct"). Free-runs as a self-wrapping torus until a master
// starts CMD_TICK, so it doubles as the Phase-3 single-chip bench test.
//
// Wiring: SDA=20 SCL=21, attention=1 (master drives HIGH during read bursts).
// Address: 0x18 + NODE_INDEX. Set NODE_INDEX per board (0..4).
#include <Arduino.h>
#include <Wire.h>

#include "protocol.h"
#include "genome.h"
#include "lenia_core.h"
#include "lenia_fixed.h"

using namespace chimera;

#ifndef NODE_INDEX
#define NODE_INDEX 4          // 0..4 -> I2C 0x18..0x1C
#endif
static constexpr int PIN_SDA = 20;
static constexpr int PIN_SCL = 21;
static constexpr int PIN_ATTN = 1;
static const uint8_t MY_ADDR = C3_ADDR_BASE + NODE_INDEX;

static LeniaStrip<FixedPolicy> strip;
static uint16_t bufA[TOTAL];
static uint16_t bufB[TOTAL];

static uint8_t haloTop[HALO_BYTES];
static uint8_t haloBottom[HALO_BYTES];
static uint8_t rxHaloTop[HALO_BYTES];
static uint8_t rxHaloBottom[HALO_BYTES];
static StripStats stats;

static volatile uint8_t rxBuf[GENOME_BYTES + CHUNK_FRAME_MAX + 8];
static volatile int rxLen = 0;
static volatile uint8_t cursorOp = CMD_GET_STATUS;
static volatile uint8_t cursorSub = 0;
static volatile uint16_t cursorChunk = 0;
static volatile bool tickRequested = false;
static volatile bool haveHaloTop = false, haveHaloBottom = false;
static volatile uint8_t nodeStatus = ST_BOOT;
static volatile uint32_t lastTickMs = 0;

// Heavy work (genome reconfigure, seed) is deferred from the ISR to loop().
static volatile bool pendingGenome = false, pendingSeed = false, pendingReset = false;
static uint8_t genomeBuf[GENOME_BYTES];
static volatile uint8_t seedPat = SEED_ORBIUM;
static volatile uint32_t seedVal = 0;

static uint8_t txFrame[CHUNK_FRAME_MAX];
static uint8_t tileBuf[(STRIP_H / 2) * (WORLD_W / 2)];

void onReceive(int n) {
    int i = 0;
    while (Wire.available() && i < (int)sizeof(rxBuf)) rxBuf[i++] = Wire.read();
    rxLen = i;
    if (rxLen == 0) return;
    switch (rxBuf[0]) {
        case CMD_TICK:
            if (rxLen >= 1 + GENOME_BYTES) { memcpy(genomeBuf, (const void*)(rxBuf + 1), GENOME_BYTES); pendingGenome = true; }
            nodeStatus = ST_COMPUTING;   // critical: set BEFORE loop computes so the barrier waits
            tickRequested = true;
            lastTickMs = millis();
            break;
        case CMD_SET_CURSOR:
            if (rxLen >= 4) {
                cursorOp = rxBuf[1];
                cursorSub = rxBuf[2];
                cursorChunk = rxBuf[3] | (rxLen >= 5 ? (rxBuf[4] << 8) : 0);
            }
            break;
        case CMD_RECV_HALO: {
            uint8_t sub = rxBuf[1];
            uint8_t* dst = sub == 0 ? rxHaloTop : rxHaloBottom;
            int got = parseFrame((const uint8_t*)rxBuf + 2, rxLen - 2, dst, HALO_BYTES);
            if (got >= 0) { if (sub == 0) haveHaloTop = true; else haveHaloBottom = true; }
            break;
        }
        case CMD_SET_GENOME:
            if (rxLen >= 1 + GENOME_BYTES) { memcpy(genomeBuf, (const void*)(rxBuf + 1), GENOME_BYTES); pendingGenome = true; }
            break;
        case CMD_SEED:
            seedPat = rxLen >= 2 ? rxBuf[1] : SEED_ORBIUM;
            seedVal = (rxLen >= 6) ? (rxBuf[2] | (rxBuf[3] << 8) | (rxBuf[4] << 16) | (rxBuf[5] << 24)) : 0;
            pendingSeed = true;
            break;
        case CMD_RESET:
            pendingReset = true;
            break;
        default:
            cursorOp = rxBuf[0];
            break;
    }
}

void onRequest() {
    switch (cursorOp) {
        case CMD_GET_STATS:
            stats.status = nodeStatus;
            Wire.write((uint8_t*)&stats, STATS_BYTES);
            break;
        case CMD_SEND_HALO: {
            const uint8_t* src = cursorSub == 0 ? haloTop : haloBottom;
            int len = buildFrame(src, HALO_BYTES, cursorChunk, txFrame);
            Wire.write(txFrame, len);
            break;
        }
        case CMD_GET_TILE: {
            int len = buildFrame(tileBuf, sizeof(tileBuf), cursorChunk, txFrame);
            Wire.write(txFrame, len);
            break;
        }
        case CMD_GET_STATUS:
        default:
            Wire.write((uint8_t)nodeStatus);
            break;
    }
}

static void buildTile() {
    int k = 0;
    for (int r = 0; r < STRIP_H; r += 2)
        for (int c = 0; c < WORLD_W; c += 2) {
            float u = strip.interiorGet(r, c) * (1.0f / 65535.0f);
            int v = (int)(u * 255.0f + 0.5f);
            tileBuf[k++] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
}

static void computeGeneration(bool fromMaster) {
    nodeStatus = ST_COMPUTING;
    if (fromMaster) {
        if (haveHaloTop)    strip.injectTopHalo(rxHaloTop);
        if (haveHaloBottom) strip.injectBottomHalo(rxHaloBottom);
    } else {
        strip.selfWrapHalos();
    }
    strip.step();
    strip.extractTopHalo(haloTop);
    strip.extractBottomHalo(haloBottom);
    stats = strip.computeStats();
    buildTile();
    haveHaloTop = haveHaloBottom = false;
    nodeStatus = ST_READY;
}

static void dumpSerial() {
    static const char ramp[] = " .:-=+*#%@";
    Serial.printf("\n[C3 %02X] gen=%lu mass=%.3f act=%.3f ent=%.3f fit=%.3f\n",
                  MY_ADDR, (unsigned long)strip.generation(), stats.mass, stats.activity,
                  stats.entropy, stats.fitness);
    for (int r = 0; r < STRIP_H; r += 2) {
        char line[WORLD_W / 2 + 1];
        int k = 0;
        for (int c = 0; c < WORLD_W; c += 2) {
            float u = strip.interiorGet(r, c) * (1.0f / 65535.0f);
            int idx = (int)(u * 9.0f);
            line[k++] = ramp[idx < 0 ? 0 : (idx > 9 ? 9 : idx)];
        }
        line[k] = '\0';
        Serial.println(line);
    }
}

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.printf("\n=== Chimera Lenia node C3 (Bank A/fixed) addr=0x%02X ===\n", MY_ADDR);

    strip.attach(bufA, bufB);
    strip.setGenome(defaultGenome(BANK_A));
    strip.seed(SEED_ORBIUM);

    pinMode(PIN_ATTN, INPUT_PULLDOWN);

    Wire.setBufferSize(WIRE_BUFFER);
    // Pass the bus frequency (100kHz) like the proven mining slave did. A 0 here
    // can leave the C3 I2C-slave timing unconfigured so it never ACKs the master.
    Wire.begin(MY_ADDR, PIN_SDA, PIN_SCL, (uint32_t)100000);
    Wire.onReceive(onReceive);
    Wire.onRequest(onRequest);

    nodeStatus = ST_READY;
    stats = strip.computeStats();
}

void loop() {
    // Apply deferred heavy commands (kept out of the I2C ISR).
    if (pendingGenome) { pendingGenome = false; strip.setGenome(deserializeGenome(genomeBuf)); }
    if (pendingReset)  { pendingReset = false; strip.clear(); }
    if (pendingSeed)   { pendingSeed = false; strip.seed(seedPat, seedVal); }

    if (tickRequested) {
        tickRequested = false;
        computeGeneration(true);
    }

    static uint32_t lastFree = 0;
    bool masterActive = (millis() - lastTickMs) < 3000 && lastTickMs != 0;
    if (!masterActive && millis() - lastFree > 80) {
        lastFree = millis();
        computeGeneration(false);
    }

    static uint32_t lastDump = 0;
    if (millis() - lastDump > 1000) { lastDump = millis(); dumpSerial(); }

    delay(1);
}
