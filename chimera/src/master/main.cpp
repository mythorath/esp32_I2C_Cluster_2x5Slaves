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
static const uint32_t SLOW_MS = 400;          // off-hot-path cadence
static const uint32_t RECONCILE_MS = 3000;    // membership re-check cadence
static const uint32_t BRINGUP_MS = 20000;     // patient power-on join window

static bool seeded[N_STRIPS] = {false};

// Assign a strip's genome + initial pattern once it's present. Bank A strips
// get the volatile two-species "instinct" genome, Bank B the persistent
// "memory" genome - this is what makes the two halves behave differently.
static void seedNode(int i) {
    genomes[i] = (stripBank(i) == BANK_A) ? instinctGenome(BANK_A) : memoryGenome(BANK_B);
    genomes[i].lineage_id = i;
    bus.setGenome(i, genomes[i]);
    bus.seed(i, (i == 0 || i == NODES_PER_BANK) ? SEED_ORBIUM : SEED_NOISE, 0xC0DE + i);
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
        delay(250);
    }
    printInventory();
}

static void gatherVitals() {
    float massSum = 0, mass1Sum = 0, entSum = 0, actSum = 0, bestFit = -1;
    int alive = 0, bestStrip = -1;
    for (int i = 0; i < N_STRIPS; i++) {
        vitals.nodeOnline[i] = bus.online(i);
        if (!bus.online(i)) continue;
        if (!bus.readStats(i, stats[i])) continue;
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

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n================================");
    Serial.println(" Chimera Lenia - MASTER");
    Serial.println("================================");

    display.begin();
    display.boot("booting...");

    bus.begin();
    delay(2500);   // longer settle for simultaneous power-on (S3 PSRAM boot is slower)

    leds.begin();
    lineage.begin();
    evolution.begin(genomes);
    router.begin();
    detector.begin();

    bringUp(BRINGUP_MS);   // patient join window + seed each node as it appears

    // Telemetry/persistence always comes up (the durable spool records even
    // headless); WiFi/Selis is an optional sink. Never a dependency of the loop.
    display.boot("link...");
    bool linked = web.begin(WIFI_SSID, WIFI_PASSWORD);
    web.attach(&vitals, stats);
    web.registerSink(lineage);     // fossil events -> durable spool + Selis
    narrator.begin(&web);
    if (linked) Serial.printf("WiFi OK (%s) -> dialing Selis\n", web.ip());
    else        Serial.println("no WiFi - running headless (spooling to flash)");

    // record network + node status for the display
    vitals.wifiOk = linked;
    strncpy(vitals.ip, linked ? web.ip() : "headless", sizeof(vitals.ip) - 1);
    for (int i = 0; i < N_STRIPS; i++) vitals.nodeOnline[i] = bus.online(i);

    // show the inventory immediately, before the first generation
    display.renderVitals(vitals, nullptr);
    delay(200);
}

void loop() {
    // ---- hot path: one generation ----
    // Bare tick (no genome) so nodes don't recompute kernel+LUT every gen;
    // evolution pushes changed genomes via CMD_SET_GENOME only when they change.
    bus.tickAll();
    bus.barrier(1500);   // demotes any stuck node; reconcile() re-admits it later
    haloEv = HaloEvents();
    router.collect(bus);
    router.route(bus, &haloEv);
    if (haloEv.seamCrossings) leds.onSeamCrossing(haloEv.seamCrossings);
    generation++;

    web.loop();

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

        evolution.update(bus, stats, genomes, vitals, generation, &leds, &lineage);

        int n = stitch.collect(bus);
        if (n) {
            detector.update(stitch, generation, vitals, &lineage, &leds, &web);
            vitals.organismsAlive = detector.aliveCount();
            display.renderField(stitch);
            web.broadcastField(stitch);
        }
        display.renderVitals(vitals, detector.newest());
        web.broadcastVitals(vitals);
        narrator.update(vitals, detector, generation);
        leds.update();

        Serial.printf("[gen %lu] online=%d mass=%.3f ent=%.3f bestFit=%.3f(s%d) orgs=%lu seam=%lu xferFails=%lu(s%d)\n",
                      (unsigned long)generation, vitals.online, vitals.worldMass, vitals.worldEntropy,
                      vitals.bestFitness, vitals.bestStrip, (unsigned long)vitals.organismsAlive,
                      (unsigned long)vitals.seamCrossings, (unsigned long)bus.bulkFails, bus.lastFailStrip);
    }
}
