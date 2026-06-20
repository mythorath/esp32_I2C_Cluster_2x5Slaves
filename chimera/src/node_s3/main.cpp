// node_s3/main.cpp - Bank B "memory" node (ESP32-S3 SuperMini, float32 biome).
//
// One node owns one horizontal strip. It is a pure I2C slave on bus 0 (the
// master drives both buses). It computes float Lenia, stages quantized halos +
// vital-signs for the master to read, keeps a PSRAM ring buffer of recent
// fields for temporal-persistence fitness, and FREE-RUNS as a self-wrapping
// torus until a master starts issuing CMD_TICK (so the same firmware works for
// the Phase-2 single-chip bench test and the full cluster).
//
// Wiring: SDA=8 SCL=9, attention=7 (master drives HIGH during read bursts).
// Address: 0x08 + NODE_INDEX. Set NODE_INDEX per board (0..4).
#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>

#include "protocol.h"
#include "genome.h"
#include "lenia_core.h"
#include "lenia_float.h"

using namespace chimera;

// ----- per-board configuration -----
#ifndef NODE_INDEX
#define NODE_INDEX 4          // 0..4 -> I2C 0x08..0x0C
#endif
static constexpr int PIN_SDA = 8;
static constexpr int PIN_SCL = 9;
static constexpr int PIN_ATTN = 7;
static const uint8_t MY_ADDR = S3_ADDR_BASE + NODE_INDEX;

// ----- engine -----
static LeniaStrip<FloatPolicy> strip;
// Temporal-fitness window. Kept small so it fits INTERNAL RAM when PSRAM is
// absent/unconfigured (PSRAM is used if available, else internal). 6 frames is
// plenty for lag-1 persistence; bump back up once PSRAM is confirmed working.
static const int HIST_N = 4;   // 2-channel float buffers are larger; keep history modest
// Bank B "memory": fraction of the previous field blended back each gen -> the
// memory bank's organisms persist and trail (vs the instinct bank's sharp ones).
static constexpr float MEMORY_ECHO = 0.10f;

// ----- staged transfer buffers (filled after each generation) -----
static uint8_t haloTop[HALO_BYTES];      // our top edge, for neighbor above
static uint8_t haloBottom[HALO_BYTES];   // our bottom edge, for neighbor below
static uint8_t rxHaloTop[HALO_BYTES];    // neighbor-above edge -> our top halo
static uint8_t rxHaloBottom[HALO_BYTES]; // neighbor-below edge -> our bottom halo
static StripStats stats;
static float persistence = 0.0f;

// ----- PSRAM history ring -----
static float* history[HIST_N] = {nullptr};
static int histHead = 0, histCount = 0;

// ----- I2C ISR <-> loop hand-off -----
static volatile uint8_t rxBuf[GENOME_BYTES + CHUNK_FRAME_MAX + 8];
static volatile int rxLen = 0;
static volatile uint8_t cursorOp = CMD_GET_STATUS;
static volatile uint8_t cursorSub = 0;       // 0=top,1=bottom (halo/tile)
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
// Downsampled 2-channel tile: [ch][DS_H_STRIP][DS_W], channel-major.
static constexpr int DS_W = WORLD_W / 2;
static constexpr int DS_H_STRIP = STRIP_H / 2;
static constexpr int TILE_PLANE = DS_W * DS_H_STRIP;
static uint8_t tileBuf[TILE_PLANE * NCH];   // 2 species planes

// --------------------------------------------------------------------------
// I2C receive: first byte is the opcode/command.
// --------------------------------------------------------------------------
void onReceive(int n) {
    int i = 0;
    while (Wire.available() && i < (int)sizeof(rxBuf)) rxBuf[i++] = Wire.read();
    rxLen = i;
    if (rxLen == 0) return;
    uint8_t op = rxBuf[0];
    switch (op) {
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
            // [op][sub][seq][len][payload...][crc8] - light enough to parse here
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
            cursorOp = op;   // a bare opcode write selects the read cursor
            break;
    }
}

// --------------------------------------------------------------------------
// I2C request: respond per the current read cursor.
// --------------------------------------------------------------------------
void onRequest() {
    switch (cursorOp) {
        case CMD_GET_STATS: {
            stats.status = nodeStatus;
            Wire.write((uint8_t*)&stats, STATS_BYTES);
            break;
        }
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

// --------------------------------------------------------------------------
static void pushHistory() {
    float* dst = history[histHead];
    if (!dst) return;
    // store the combined (both-species) field; persistence + the Phase-2 memory
    // echo operate on the combined intensity.
    for (int r = 0; r < STRIP_H; r++)
        for (int c = 0; c < WORLD_W; c++)
            dst[r * WORLD_W + c] = strip.interiorGet(0, r, c) + strip.interiorGet(1, r, c);
    histHead = (histHead + 1) % HIST_N;
    if (histCount < HIST_N) histCount++;
}

// Temporal persistence: lag-1 correlation between the two most recent frames.
static float computePersistence() {
    if (histCount < 2) return 0.0f;
    int a = (histHead - 1 + HIST_N) % HIST_N;
    int b = (histHead - 2 + HIST_N) % HIST_N;
    const float* fa = history[a];
    const float* fb = history[b];
    int n = STRIP_H * WORLD_W;
    double ma = 0, mb = 0;
    for (int i = 0; i < n; i++) { ma += fa[i]; mb += fb[i]; }
    ma /= n; mb /= n;
    double num = 0, da = 0, db = 0;
    for (int i = 0; i < n; i++) {
        double xa = fa[i] - ma, xb = fb[i] - mb;
        num += xa * xb; da += xa * xa; db += xb * xb;
    }
    if (da < 1e-9 || db < 1e-9) return 0.0f;
    double c = num / sqrt(da * db);
    return (float)(c < 0 ? 0 : (c > 1 ? 1 : c));
}

static void buildTile() {
    for (int ch = 0; ch < NCH; ch++) {
        int k = ch * TILE_PLANE;
        for (int r = 0; r < STRIP_H; r += 2)
            for (int c = 0; c < WORLD_W; c += 2) {
                float u = strip.interiorGet(ch, r, c);
                int v = (int)(u * 255.0f + 0.5f);
                tileBuf[k++] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
    }
}

static void computeGeneration(bool fromMaster) {
    nodeStatus = ST_COMPUTING;
    if (fromMaster) {
        // Master-mediated: inject neighbor halos staged via CMD_RECV_HALO.
        if (haveHaloTop)    strip.injectTopHalo(rxHaloTop);
        if (haveHaloBottom) strip.injectBottomHalo(rxHaloBottom);
    } else {
        strip.selfWrapHalos();   // free-run: single-chip torus
    }
    strip.step();
    strip.blendPrevious(MEMORY_ECHO);   // Bank B "memory": persistent, trailing structures
    strip.extractTopHalo(haloTop);
    strip.extractBottomHalo(haloBottom);
    pushHistory();
    persistence = computePersistence();
    stats = strip.computeStats();
    stats.fitness += 0.3f * persistence * (0.5f + stats.activity);
    buildTile();
    haveHaloTop = haveHaloBottom = false;
    nodeStatus = ST_READY;
}

// --------------------------------------------------------------------------
static void dumpSerial() {
    static const char rampP[] = " .:-=+*";   // prey (species 0)
    static const char rampD[] = " oO0@#%";   // predator (species 1, wins ties)
    Serial.printf("\n[S3 %02X] gen=%lu mass=%.3f sp1=%.3f act=%.3f ent=%.3f persist=%.3f fit=%.3f\n",
                  MY_ADDR, (unsigned long)strip.generation(), stats.mass, stats.mass1,
                  stats.activity, stats.entropy, persistence, stats.fitness);
    for (int r = 0; r < STRIP_H; r += 2) {
        char line[WORLD_W / 2 + 1];
        int k = 0;
        for (int c = 0; c < WORLD_W; c += 2) {
            float p = strip.interiorGet(0, r, c);
            float d = strip.interiorGet(1, r, c);
            if (d >= p) { int i = (int)(d * 6.0f); line[k++] = rampD[i < 0 ? 0 : (i > 6 ? 6 : i)]; }
            else        { int i = (int)(p * 6.0f); line[k++] = rampP[i < 0 ? 0 : (i > 6 ? 6 : i)]; }
        }
        line[k] = '\0';
        Serial.println(line);
    }
}

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.printf("\n=== Chimera Lenia node S3 (Bank B/float) addr=0x%02X ===\n", MY_ADDR);

    // Strip buffers: prefer PSRAM if present, else internal RAM (they fit).
    bool psram = psramFound();
    Serial.printf("PSRAM: %s\n", psram ? "present" : "absent (using internal RAM)");
    const size_t bufBytes = (size_t)NCH * TOTAL * sizeof(float);   // both species
    float* bufA = psram ? (float*)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM) : nullptr;
    float* bufB = psram ? (float*)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM) : nullptr;
    if (!bufA) bufA = (float*)malloc(bufBytes);
    if (!bufB) bufB = (float*)malloc(bufBytes);
    strip.attach(bufA, bufB);
    strip.setGenome(memoryGenome(BANK_B));   // Bank B = slow/persistent two-species ecology
    strip.seed(SEED_ORBIUM);

    // History ring (best-effort): PSRAM if present, else internal; null entries
    // are skipped by pushHistory(), so the node still runs if allocation fails.
    for (int i = 0; i < HIST_N; i++) {
        history[i] = psram ? (float*)heap_caps_malloc(STRIP_H * WORLD_W * sizeof(float), MALLOC_CAP_SPIRAM) : nullptr;
        if (!history[i]) history[i] = (float*)malloc(STRIP_H * WORLD_W * sizeof(float));
    }

    pinMode(PIN_ATTN, INPUT_PULLDOWN);

    Wire.setBufferSize(WIRE_BUFFER);
    Wire.begin(MY_ADDR, PIN_SDA, PIN_SCL, (uint32_t)100000);   // pass bus freq (matches master)
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

    // Master-driven generation (polled-barrier flow).
    if (tickRequested) {
        tickRequested = false;
        computeGeneration(true);
    }

    // Free-run when no master has ticked us recently (Phase-2 bench mode).
    static uint32_t lastFree = 0;
    bool masterActive = (millis() - lastTickMs) < 3000 && lastTickMs != 0;
    if (!masterActive && millis() - lastFree > 60) {
        lastFree = millis();
        computeGeneration(false);
    }

    // Periodic serial visualization.
    static uint32_t lastDump = 0;
    if (millis() - lastDump > 1000) { lastDump = millis(); dumpSerial(); }

    delay(1);
}
