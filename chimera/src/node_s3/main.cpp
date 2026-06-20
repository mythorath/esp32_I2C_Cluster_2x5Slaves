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
#include <esp_bt.h>

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
// Temporal-fitness window. Sized to fit INTERNAL RAM (PSRAM does not init on
// these boards): 4 combined-field frames ~= 80 KB. Only lag-1 is used today.
static const int HIST_N = 4;
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
// Temporal persistence: lag-1 Pearson correlation between the two most recent
// combined-field frames, computed in a SINGLE pass via the sum-of-products form
// (no separate mean pass). fa = newest frame, fb = previous frame.
static float persistenceOnePass(const float* fa, const float* fb) {
    int n = STRIP_H * WORLD_W;
    double sa = 0, sb = 0, sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < n; i++) {
        double a = fa[i], b = fb[i];
        sa += a; sb += b; sab += a * b; saa += a * a; sbb += b * b;
    }
    double num = n * sab - sa * sb;
    double da = n * saa - sa * sa;
    double db = n * sbb - sb * sb;
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

#ifdef PROFILE_GEN
// Per-stage timing accumulators (microseconds) summed across a 1 s window, then
// averaged per generation in the periodic profile print.
static uint32_t profStep = 0, profBlend = 0, profHalo = 0, profHist = 0, profStats = 0, profTile = 0;
static uint32_t profGens = 0;
#define PROF_T0() uint32_t _pt = micros()
#define PROF_ACC(field) do { uint32_t _now = micros(); field += _now - _pt; _pt = _now; } while (0)
#define PROF_GEN_DONE() do { profGens++; } while (0)
#else
#define PROF_T0() do {} while (0)
#define PROF_ACC(field) do {} while (0)
#define PROF_GEN_DONE() do {} while (0)
#endif

static void computeGeneration(bool fromMaster) {
    nodeStatus = ST_COMPUTING;
    if (fromMaster) {
        // Master-mediated: inject neighbor halos staged via CMD_RECV_HALO.
        if (haveHaloTop)    strip.injectTopHalo(rxHaloTop);
        if (haveHaloBottom) strip.injectBottomHalo(rxHaloBottom);
    } else {
        strip.selfWrapHalos();   // free-run: single-chip torus
    }
    PROF_T0();
    strip.step();
    PROF_ACC(profStep);
    // Fused: memory-echo blend + stats + combined-field history write in one
    // interior pass. The combined field lands directly in the new history slot.
    float* newFrame = history[histHead];
    stats = strip.blendAndDigest(MEMORY_ECHO, newFrame);
    PROF_ACC(profBlend);
    strip.extractTopHalo(haloTop);
    strip.extractBottomHalo(haloBottom);
    PROF_ACC(profHalo);
    // Persistence vs the previous frame, then advance the ring. Both slots must
    // be non-null (allocation can fail) and we need at least one prior frame.
    const float* prevFrame = history[(histHead - 1 + HIST_N) % HIST_N];
    persistence = (newFrame && prevFrame && histCount >= 1)
                      ? persistenceOnePass(newFrame, prevFrame) : 0.0f;
    if (newFrame) {
        histHead = (histHead + 1) % HIST_N;
        if (histCount < HIST_N) histCount++;
    }
    stats.fitness += 0.3f * persistence * (0.5f + stats.activity);
    PROF_ACC(profHist);
    buildTile();
    PROF_ACC(profTile);
    PROF_GEN_DONE();
    haveHaloTop = haveHaloBottom = false;
    nodeStatus = ST_READY;
}

#ifdef PROFILE_GEN
// Average per-generation stage breakdown over the elapsed window, then reset.
static void dumpProfile() {
    uint32_t g = profGens ? profGens : 1;
    Serial.printf("[S3 %02X] prof gens=%lu step=%luus blend=%luus halo=%luus hist=%luus stats=%luus tile=%luus total=%luus\n",
                  MY_ADDR, (unsigned long)profGens,
                  (unsigned long)(profStep / g), (unsigned long)(profBlend / g),
                  (unsigned long)(profHalo / g), (unsigned long)(profHist / g),
                  (unsigned long)(profStats / g), (unsigned long)(profTile / g),
                  (unsigned long)((profStep + profBlend + profHalo + profHist + profStats + profTile) / g));
    profStep = profBlend = profHalo = profHist = profStats = profTile = profGens = 0;
}
#endif

// --------------------------------------------------------------------------
#ifndef CLUSTER_MODE
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
#endif

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.printf("\n=== Chimera Lenia node S3 (Bank B/float) addr=0x%02X ===\n", MY_ADDR);

    // Slaves never use the radios. WiFi is never started (idle), but the BT
    // controller reserves heap at boot - release it so the float buffers and
    // history ring have more internal RAM headroom. Safe when BT is unused.
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);

    // Memory placement policy: the strip ping-pong buffers are the hot gather-
    // convolution working set, so FORCE them into fast internal SRAM (PSRAM gather
    // reads would stall step()). The history ring is cold/sequential, so prefer
    // PSRAM and only fall back to internal if PSRAM is absent.
    bool psram = psramFound();
    Serial.printf("PSRAM: %s\n", psram ? "present" : "absent (using internal RAM)");
    const size_t bufBytes = (size_t)NCH * TOTAL * sizeof(float);   // both species
    float* bufA = (float*)heap_caps_malloc(bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float* bufB = (float*)heap_caps_malloc(bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!bufA) bufA = (float*)malloc(bufBytes);   // last-ditch fallback
    if (!bufB) bufB = (float*)malloc(bufBytes);
    strip.attach(bufA, bufB);
    strip.setGenome(memoryGenome(BANK_B));   // Bank B = slow/persistent two-species ecology
    strip.seed(SEED_ORBIUM);

    // History ring (best-effort): PSRAM if present, else internal; null entries
    // are skipped by computeGeneration(), so the node still runs if allocation fails.
    const size_t frameBytes = (size_t)STRIP_H * WORLD_W * sizeof(float);
    for (int i = 0; i < HIST_N; i++) {
        history[i] = psram ? (float*)heap_caps_malloc(frameBytes, MALLOC_CAP_SPIRAM) : nullptr;
        if (!history[i]) history[i] = (float*)heap_caps_malloc(frameBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    Serial.printf("[mem] freeHeap=%u internalFree=%u largestInternal=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    pinMode(PIN_ATTN, INPUT_PULLDOWN);

    Wire.setBufferSize(WIRE_BUFFER);
    Wire.begin(MY_ADDR, PIN_SDA, PIN_SCL, I2C_HZ);   // shared master/node bus clock (protocol.h)
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

#ifdef PROFILE_GEN
    // Per-stage timing breakdown (averaged over the last second).
    static uint32_t lastProf = 0;
    if (millis() - lastProf > 1000) { lastProf = millis(); dumpProfile(); }
#endif

#ifndef CLUSTER_MODE
    // Bench-only ASCII visualization. Skipped in cluster mode: the master owns
    // visualization and this raster is pure USB overhead under barrier sync.
    static uint32_t lastDump = 0;
    if (millis() - lastDump > 1000) { lastDump = millis(); dumpSerial(); }
#endif

    // Under master barrier sync the loop must spin tight so ST_READY is observed
    // ASAP; the 1 ms nap only matters for the free-run bench cadence.
    if (!masterActive) delay(1);
}
