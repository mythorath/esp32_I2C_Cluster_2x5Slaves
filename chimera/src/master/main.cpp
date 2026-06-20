// master/main.cpp - Chimera Lenia orchestrator (ESP32 WROOM-32 DevKit).
//
// Generation loop across both banks using a software polled barrier (nodes
// can't clock-stretch). Per generation:
//   1. broadcast CMD_TICK to all online nodes (both banks compute in parallel)
//   2. wait until ALL nodes report ST_READY (barrier)
//   3. read each node's staged boundary edges (halos)
//   4. route halos to ring neighbors (transcoded across the two A<->B seams)
//   5. write neighbor halos back, staged for the next tick
// Off the hot path (throttled): gather stats -> homeostasis + island evolution;
// stitch tiles -> detect/name organisms; render OLED/ST7789 + web viz; pulse
// transmission-line LEDs on real events.
#include <Arduino.h>
#include <esp_system.h>   // esp_random() for staggered/noise reseeds + coupling

#include "protocol.h"
#include "genome.h"
#include "topology.h"
#include "bus_manager.h"
#include "halo_router.h"
#include "stitch.h"
#include "display.h"
#include "telemetry.h"
#include "world_state.h"
#include "evolution.h"
#include "detect.h"
#include "lineage.h"
#include "narrator.h"
#include "led_events.h"

#if __has_include("secrets.h")
#include "secrets.h"
#define HAVE_SECRETS 1
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define HAVE_SECRETS 0
#endif

using namespace chimera;

static BusManager bus;
static HaloRouter router;
static Stitch stitch;
static Display display;
static TelemetryClient web;   // outbound client to Selis (kept name `web` for call sites)
static Evolution evolution;
static Detector detector;
static Lineage lineage;
static Narrator narrator;
static LedEvents leds;

static Genome genomes[N_STRIPS];
static StripStats stats[N_STRIPS];
static WorldVitals vitals;
static HaloEvents haloEv;

static uint32_t generation = 0;
static uint32_t lastSlow = 0;
static uint32_t lastReconcile = 0;
static uint32_t lastCouplingGen = 0;
static const uint32_t SLOW_MS = 250;          // off-hot-path cadence (field viz ~4 Hz)
static const uint32_t RECONCILE_MS = 3000;    // membership re-check cadence
static const uint32_t BRINGUP_MS = 20000;     // patient power-on join window
static const uint32_t COUPLING_EVERY = 24;    // gens between inter-hemisphere "breaths" (~4 min)

static bool seeded[N_STRIPS] = {false};

// Assign a strip's genome + initial pattern once it's present. Bank A strips
// get the volatile two-species "instinct" genome, Bank B the persistent
// "memory" genome - this is what makes the two halves behave differently.
static void seedNode(int i) {
    genomes[i] = (stripBank(i) == BANK_A) ? instinctGenome(BANK_A) : memoryGenome(BANK_B);
    genomes[i].lineage_id = i;
    bus.setGenome(i, genomes[i]);
    // Stagger the encounter phase across the torus (even horizontal spread) so
    // chases don't all start in lockstep. seedVal is the Orbium column offset.
    bus.seed(i, SEED_ORBIUM, (uint32_t)(i * (WORLD_W / N_STRIPS)));
    seeded[i] = true;
}

static void printInventory() {
    Serial.println("\n--- cluster inventory ---");
    for (int i = 0; i < N_STRIPS; i++)
        Serial.printf("  strip %d bank %c bus %d addr 0x%02X %s\n",
                      i, stripBank(i) == BANK_A ? 'A' : 'B', stripBus(i), stripAddr(i),
                      bus.online(i) ? "ONLINE" : "--");
    Serial.printf("  online: %d/%d\n", bus.onlineCount(), N_STRIPS);
}

// Re-probe offline strips so late or brownout-recovered nodes hot-join the
// running world; seed each one the first time it appears. Returns # that joined.
// (Online nodes that fail a tick/halo are demoted by the bus/barrier, so they
// land back here and get re-admitted when they recover.)
static int reconcile() {
    int joined = 0;
    for (int i = 0; i < N_STRIPS; i++) {
        if (bus.online(i)) continue;
        if (bus.probe(i)) {
            if (!seeded[i]) seedNode(i);
            joined++;
        }
    }
    return joined;
}

// Patient power-on bring-up: keep probing for a window so boards that boot
// slower (S3 PSRAM init) or recover from inrush brownout still get admitted,
// instead of being frozen out by a single fixed-delay scan.
static void bringUp(uint32_t windowMs) {
    Serial.println("\n--- cluster bring-up ---");
    uint32_t start = millis();
    int last = -1;
    while (millis() - start < windowMs) {
        for (int i = 0; i < N_STRIPS; i++)
            if (!bus.online(i) && bus.probe(i) && !seeded[i]) seedNode(i);
        int n = bus.onlineCount();
        if (n != last) {
            last = n;
            Serial.printf("  online %d/%d  (%.1fs)\n", n, N_STRIPS, (millis() - start) / 1000.0f);
        }
        if (n == N_STRIPS) break;
        web.service();
        delay(250);
    }
    printInventory();
}

static float prevComX[N_STRIPS];
static float prevComY[N_STRIPS];
static bool prevComOk[N_STRIPS] = {false};

static void gatherVitals() {
    float massSum = 0, mass1Sum = 0, entSum = 0, actSum = 0, bestFit = -1;
    int alive = 0, bestStrip = -1;
    for (int i = 0; i < N_STRIPS; i++) {
        vitals.nodeOnline[i] = bus.online(i);
        if (!bus.online(i)) continue;
        if (!bus.readStats(i, stats[i])) continue;
        // Reward visible motion so island evolution selects gliders, not static blobs.
        if (!isnan(stats[i].com_x) && !isnan(stats[i].com_y) && prevComOk[i]) {
            float dx = stats[i].com_x - prevComX[i];
            float dy = stats[i].com_y - prevComY[i];
            float motion = sqrtf(dx * dx + dy * dy) / (float)STRIP_H;
            stats[i].fitness += 2.5f * motion;
        }
        if (!isnan(stats[i].com_x) && !isnan(stats[i].com_y)) {
            prevComX[i] = stats[i].com_x;
            prevComY[i] = stats[i].com_y;
            prevComOk[i] = true;
        }
        massSum += stats[i].mass; mass1Sum += stats[i].mass1;
        entSum += stats[i].entropy; actSum += stats[i].activity;
        alive++;
        if (stats[i].fitness > bestFit) { bestFit = stats[i].fitness; bestStrip = i; }
    }
    vitals.generation = generation;
    vitals.online = bus.onlineCount();
    vitals.worldMass = alive ? massSum / alive : 0;
    vitals.worldMass1 = alive ? mass1Sum / alive : 0;
    vitals.worldEntropy = alive ? entSum / alive : 0;
    vitals.worldActivity = alive ? actSum / alive : 0;
    vitals.bestFitness = bestFit < 0 ? 0 : bestFit;
    vitals.bestStrip = bestStrip;
    vitals.seamCrossings += haloEv.seamCrossings;
}

// ---- full environment reset ------------------------------------------------
// The world uses random seeds + open-ended evolution, so it can settle into a
// "stuck" state with no autonomous way out. A reset reseeds every strip back to
// its bank personality and zeroes the master's evolutionary state, giving a
// clean t=0 world. Requests are QUEUED (serial or Selis "cmd") and executed here
// between generations so the hot I2C path is never interrupted.
static volatile bool g_resetPending = false;
static uint8_t g_resetPattern = SEED_ORBIUM;
static bool g_resetClearHistory = false;

static void requestReset(uint8_t pattern, bool clearHistory) {
    g_resetPattern = pattern;
    g_resetClearHistory = clearHistory;
    g_resetPending = true;
}

static void environmentReset(uint8_t pattern, bool clearHistory) {
    Serial.printf("\n*** ENVIRONMENT RESET (pattern=%s, clearHistory=%d) ***\n",
                  pattern == SEED_NOISE ? "noise" : "orbium", clearHistory);
    for (int i = 0; i < N_STRIPS; i++) {
        prevComOk[i] = false;
        if (!bus.online(i)) { seeded[i] = false; continue; }
        genomes[i] = (stripBank(i) == BANK_A) ? instinctGenome(BANK_A) : memoryGenome(BANK_B);
        genomes[i].lineage_id = i;
        bus.setGenome(i, genomes[i]);
        // Orbium: staggered phase per strip. Noise: a fresh random field per strip.
        uint32_t sv = (pattern == SEED_NOISE) ? (esp_random() + i)
                                              : (uint32_t)(i * (WORLD_W / N_STRIPS));
        bus.seed(i, pattern, sv);
        seeded[i] = true;
    }
    generation = 0;
    lastCouplingGen = 0;
    evolution.reset();
    detector.reset();
    lineage.begin();                 // clear the RAM fossil ring
    router.setCoupling(1.0f);
    if (clearHistory) { web.events().clear(); web.vitals().clear(); }
    vitals.births = vitals.deaths = vitals.migrations = vitals.seamCrossings = 0;
    vitals.organismsAlive = 0;
    vitals.coupling = 1.0f;
    vitals.generation = 0;
    lineage.record(LIN_RESET, 0, -1, -1, 0, 0, 0);   // first fossil of the new world
    web.pushEvent("system", clearHistory ? "environment reset (history cleared)"
                                         : "environment reset");
}

// Selis -> master operator command (queues work; runs off the hot path).
static void onTelemetryCommand(const char* name, const char* pattern, bool clearHistory, void*) {
    if (strcmp(name, "reset_environment") == 0 || strcmp(name, "reset") == 0) {
        uint8_t pat = (pattern && strcmp(pattern, "noise") == 0) ? SEED_NOISE : SEED_ORBIUM;
        requestReset(pat, clearHistory);
    } else {
        Serial.printf("[telemetry] unknown cmd '%s'\n", name);
    }
}

// USB serial console: "RESET", "RESET noise", "RESET clear" (combine as needed).
static void pollSerialCommand() {
    static char line[40];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            line[len] = '\0';
            if (strncmp(line, "RESET", 5) == 0 || strncmp(line, "reset", 5) == 0) {
                uint8_t pat = strstr(line, "noise") ? SEED_NOISE : SEED_ORBIUM;
                bool clear = strstr(line, "clear") != nullptr;
                requestReset(pat, clear);
                Serial.printf("[serial] reset queued (pattern=%s clear=%d)\n",
                              pat == SEED_NOISE ? "noise" : "orbium", clear);
            }
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n================================");
    Serial.println(" Chimera Lenia - MASTER");
    Serial.println("================================");

    display.begin();
    display.boot("wifi...");

    // Claim the big contiguous RAM buffers on a fresh, unfragmented heap BEFORE
    // WiFi brings up its pools. The halo router (critical sim path) and the
    // organism detector each need large blocks; allocating them post-WiFi could
    // fail, which either silently disabled detection (orgs stuck at 0) or - worse
    // - left a null halo buffer that crashed the hot path. Order: detector then
    // router so both land before WiFi.
    detector.begin();
    router.begin();

    // Start WiFi early so it can associate while the cluster bring-up runs.
    bool linked = web.begin(WIFI_SSID, WIFI_PASSWORD);
    web.attach(&vitals, stats);
    web.registerSink(lineage);
    web.onCommand(onTelemetryCommand, nullptr);

    display.boot("cluster...");
    bus.begin();
    delay(2500);   // longer settle for simultaneous power-on (S3 PSRAM boot is slower)

    leds.begin();
    lineage.begin();
    evolution.begin(genomes);

    Serial.printf("[mem] post-init freeHeap=%u largestBlock=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    bringUp(BRINGUP_MS);   // patient join window + seed each node as it appears

    narrator.begin(&web);
    if (linked) Serial.printf("Selis telemetry ready (%s)\n", web.ip());
    else        Serial.println("no WiFi yet - running headless (will retry, spooling to flash)");

    // record network + node status for the display
    vitals.wifiOk = web.wifiLinked();
    strncpy(vitals.ip, vitals.wifiOk ? web.ip() : "headless", sizeof(vitals.ip) - 1);
    for (int i = 0; i < N_STRIPS; i++) vitals.nodeOnline[i] = bus.online(i);

    // show the inventory immediately, before the first generation
    display.renderVitals(vitals, nullptr);
    delay(200);
}

void loop() {
    web.service();

    // ---- operator-requested environment reset (between generations) ----
    pollSerialCommand();
    if (g_resetPending) {
        g_resetPending = false;
        environmentReset(g_resetPattern, g_resetClearHistory);
    }

    // ---- hot path: one generation ----
    // Bare tick (no genome) so nodes don't recompute kernel+LUT every gen;
    // evolution pushes changed genomes via CMD_SET_GENOME only when they change.
#ifdef PROFILE_GEN
    uint32_t _pt0 = micros();
#endif
    bus.tickAll();
    web.service();
#ifdef PROFILE_GEN
    uint32_t _ptTick = micros();
#endif
    bus.barrier(1500);   // demotes any stuck node; reconcile() re-admits it later
    web.service();
#ifdef PROFILE_GEN
    uint32_t _ptBar = micros();
#endif
    haloEv = HaloEvents();
    router.collect(bus);
    web.service();
    router.route(bus, &haloEv);
#ifdef PROFILE_GEN
    uint32_t _ptHalo = micros();
    Serial.printf("[prof gen %lu] tick=%luus barrier=%luus halo=%luus total=%luus\n",
                  (unsigned long)generation,
                  (unsigned long)(_ptTick - _pt0), (unsigned long)(_ptBar - _ptTick),
                  (unsigned long)(_ptHalo - _ptBar), (unsigned long)(_ptHalo - _pt0));
#endif
    if (haloEv.seamCrossings) leds.onSeamCrossing(haloEv.seamCrossings);
    generation++;

    // ---- inter-hemisphere "breathing": vary seam coupling over time ----
    if (generation - lastCouplingGen >= COUPLING_EVERY) {
        lastCouplingGen = generation;
        float c = 0.35f + (esp_random() & 0xFFFF) / 65535.0f * 0.65f;   // [0.35, 1.0]
        router.setCoupling(c);
        vitals.coupling = c;
        leds.onCouplingChange(c);
    }

    web.service();

    // ---- membership reconciliation: hot-join late/recovered nodes ----
    if (millis() - lastReconcile > RECONCILE_MS) {
        lastReconcile = millis();
        int joined = reconcile();
        if (joined) Serial.printf("[reconcile] %d node(s) joined -> %d/%d online\n",
                                  joined, bus.onlineCount(), N_STRIPS);
    }

    // ---- off hot path ----
    if (millis() - lastSlow > SLOW_MS) {
        lastSlow = millis();
        gatherVitals();

        // Refresh network status on the LCD once WiFi comes up (or drops).
        bool wifiNow = web.wifiLinked();
        if (wifiNow != vitals.wifiOk) {
            vitals.wifiOk = wifiNow;
            strncpy(vitals.ip, wifiNow ? web.ip() : "headless", sizeof(vitals.ip) - 1);
        }

        evolution.update(bus, stats, genomes, vitals, generation, &leds, &lineage);

        int n = stitch.collect(bus);
        web.service();
        if (n) {
            detector.update(stitch, generation, vitals, &lineage, &leds, &web);
            vitals.organismsAlive = detector.aliveCount();
            display.renderField(stitch);
            web.broadcastField(stitch);
        }
        display.renderVitals(vitals, detector.newest());
        web.broadcastVitals(vitals);
        web.service();
        narrator.update(vitals, detector, generation);
        leds.update();

        Serial.printf("[gen %lu] online=%d mass=%.3f ent=%.3f bestFit=%.3f(s%d) orgs=%lu seam=%lu xferFails=%lu(s%d)\n",
                      (unsigned long)generation, vitals.online, vitals.worldMass, vitals.worldEntropy,
                      vitals.bestFitness, vitals.bestStrip, (unsigned long)vitals.organismsAlive,
                      (unsigned long)vitals.seamCrossings, (unsigned long)bus.bulkFails, bus.lastFailStrip);
    }

    web.service();
}
