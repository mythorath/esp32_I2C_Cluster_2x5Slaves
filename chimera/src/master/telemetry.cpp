#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "telemetry.h"
#include "topology.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef SELIS_HOST
#define SELIS_HOST "selis.local"
#endif
#ifndef SELIS_PORT
#define SELIS_PORT 8787
#endif
#ifndef SELIS_PATH
#define SELIS_PATH "/ingest"
#endif
#ifndef VITALS_SNAP_MS
#define VITALS_SNAP_MS 30000
#endif
#define FW_VERSION "chimera-2species-1"

namespace chimera {

static WebSocketsClient ws;
static TelemetryClient* g_self = nullptr;
static int replayBudget_ = 0;   // cap durable replay per handshake (keeps WS alive)

static void pumpWs() { ws.loop(); }

void TelemetryClient::service() { pumpWs(); }

// ----- helpers to emit the JSON envelopes (shared by live + replay) -----
static void formatEventText(const LineageEvent& e, char* out, size_t cap) {
    switch (e.type) {
        case LIN_BIRTH:
            snprintf(out, cap, "organism #%u born on strip %d", e.organismId, (int)e.fromStrip);
            break;
        case LIN_DEATH:
            snprintf(out, cap, "organism #%u vanished", e.organismId);
            break;
        case LIN_COLONIZE:
            snprintf(out, cap, "strip %d colonized from strip %d", (int)e.toStrip, (int)e.fromStrip);
            break;
        case LIN_MIGRATION:
            snprintf(out, cap, "genome migrated %d -> %d across the seam", (int)e.fromStrip, (int)e.toStrip);
            break;
        case LIN_WILDCARD:
            snprintf(out, cap, "wildcard reseed on strip %d", (int)e.toStrip);
            break;
        case LIN_MUTATE:
            snprintf(out, cap, "mutation on strip %d", (int)e.toStrip);
            break;
        case LIN_RESET:
            snprintf(out, cap, "environment reset - world reseeded");
            break;
        default:
            snprintf(out, cap, "%s %d->%d", Lineage::typeName(e.type), (int)e.fromStrip, (int)e.toStrip);
            break;
    }
}

static void sendEventJson(uint32_t eseq, const LineageEvent& e) {
    if (!ws.isConnected()) return;
    StaticJsonDocument<320> d;
    d["t"] = "event";
    d["eseq"] = eseq;
    d["gen"] = e.gen;
    d["kind"] = Lineage::typeName(e.type);
    d["fromStrip"] = e.fromStrip;
    d["toStrip"] = e.toStrip;
    d["lineageId"] = e.lineageId;
    d["organismId"] = e.organismId;
    d["fitness"] = e.fitness;
    char text[80];
    formatEventText(e, text, sizeof text);
    d["text"] = text;
    String s; serializeJson(d, s); ws.sendTXT(s);
    pumpWs();
}

static void sendSnapJson(uint32_t vseq, const VitalsSnap& v) {
    if (!ws.isConnected()) return;
    StaticJsonDocument<384> d;
    d["t"] = "snap";
    d["vseq"] = vseq;
    d["gen"] = v.gen;
    d["mass"] = v.mass;
    d["activity"] = v.activity;
    d["entropy"] = v.entropy;
    d["bestFitness"] = v.bestFitness;
    d["bestStrip"] = v.bestStrip;
    d["coupling"] = v.coupling;
    d["organisms"] = v.organismsAlive;
    d["births"] = v.births;
    d["deaths"] = v.deaths;
    d["migrations"] = v.migrations;
    d["seamCrossings"] = v.seamCrossings;
    d["online"] = v.online;
    String s; serializeJson(d, s); ws.sendTXT(s);
}

// replay trampolines (LogStream::ReplayCb)
static void replayEventCb(uint32_t seq, const uint8_t* payload, uint8_t len, void*) {
    if (len < sizeof(LineageEvent)) return;
    if (replayBudget_-- <= 0) return;
    LineageEvent e; memcpy(&e, payload, sizeof(e));
    sendEventJson(seq, e);
    pumpWs();
}
static void replaySnapCb(uint32_t seq, const uint8_t* payload, uint8_t len, void*) {
    if (len < sizeof(VitalsSnap)) return;
    if (replayBudget_-- <= 0) return;
    VitalsSnap v; memcpy(&v, payload, sizeof(v));
    sendSnapJson(seq, v);
    pumpWs();
}

static void lineageSinkTrampoline(const LineageEvent& e, void* ctx) {
    ((TelemetryClient*)ctx)->onFossil(e);
}

static void sendHello() {
    StaticJsonDocument<256> d;
    d["t"] = "hello";
    d["firmware"] = FW_VERSION;
    d["nStrips"] = N_STRIPS;
    d["dsW"] = DS_W;
    d["dsH"] = DS_H;
    d["evSeqMax"] = g_self->events().highestSeq();
    d["vitSeqMax"] = g_self->vitals().highestSeq();
    String s; serializeJson(d, s); ws.sendTXT(s);
}

static void onWsText(uint8_t* payload, size_t len) {
    StaticJsonDocument<384> d;
    if (deserializeJson(d, payload, len)) return;
    const char* t = d["t"] | "";
    if (strcmp(t, "hello") == 0) {
        uint32_t ackEv = d["ackEv"] | 0;
        uint32_t ackVit = d["ackVit"] | 0;
        // replay the gap from flash so Selis catches up on everything it missed
        g_self->events().replaySince(ackEv, replayEventCb, g_self);
        g_self->vitals().replaySince(ackVit, replaySnapCb, g_self);
    } else if (strcmp(t, "ack") == 0) {
        g_self->events().ackUpTo(d["ev"] | 0);
        g_self->vitals().ackUpTo(d["vit"] | 0);
    } else if (strcmp(t, "cmd") == 0) {
        // Operator command channel (docs/cluster-telemetry-and-persistence.md sec 4.2).
        // The handler only queues a request; the master runs it off the hot I2C path.
        const char* name = d["name"] | "";
        const char* pattern = d["args"]["pattern"] | "orbium";
        bool clearHistory = d["args"]["clear_history"] | false;
        Serial.printf("[telemetry] cmd '%s' pattern=%s clearHistory=%d\n",
                      name, pattern, clearHistory);
        g_self->dispatchCommand(name, pattern, clearHistory);
    }
}

static void onWsEvent(WStype_t type, uint8_t* payload, size_t len) {
    switch (type) {
        case WStype_CONNECTED:
            Serial.println("[telemetry] Selis WS connected");
            replayBudget_ = 32;   // replay a small gap per connect; rest on later acks
            sendHello();
            break;
        case WStype_DISCONNECTED:
            Serial.println("[telemetry] Selis WS disconnected");
            break;
        case WStype_TEXT:
            onWsText(payload, len);
            break;
        default:
            break;
    }
}

static const char* wifiStatusName(wl_status_t s) {
    switch (s) {
        case WL_IDLE_STATUS: return "IDLE";
        case WL_NO_SSID_AVAIL: return "NO_SSID";
        case WL_SCAN_COMPLETED: return "SCAN_DONE";
        case WL_CONNECTED: return "CONNECTED";
        case WL_CONNECT_FAILED: return "CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "LOST";
        case WL_DISCONNECTED: return "DISCONNECTED";
        default: return "UNKNOWN";
    }
}

void TelemetryClient::startWs() {
    if (wsStarted_) return;
    String host = SELIS_HOST;
    if (host.endsWith(".local")) {
        if (MDNS.begin("chimera-master")) {
            IPAddress r = MDNS.queryHost(host.substring(0, host.length() - 6));
            if (r) host = r.toString();
        }
    }
    ws.begin(host.c_str(), (uint16_t)SELIS_PORT, SELIS_PATH);
    ws.onEvent([](WStype_t t, uint8_t* p, size_t l) { onWsEvent(t, p, l); });
    ws.setReconnectInterval(3000);
    wsStarted_ = true;
    Serial.printf("dialing Selis ws://%s:%d%s\n", host.c_str(), SELIS_PORT, SELIS_PATH);
}

bool TelemetryClient::connectWifi(uint32_t waitMs) {
    if (!ssid_[0]) return false;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid_, pass_);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < waitMs) {
        pumpWs();
        delay(10);
    }
    wifiOk_ = WiFi.status() == WL_CONNECTED;
    if (wifiOk_) {
        strncpy(ip_, WiFi.localIP().toString().c_str(), sizeof(ip_) - 1);
        Serial.printf("WiFi OK (%s) RSSI=%d\n", ip_, WiFi.RSSI());
        startWs();
        return true;
    }
    Serial.printf("WiFi failed (%s) after %lums\n", wifiStatusName(WiFi.status()),
                  (unsigned long)waitMs);
    int n = WiFi.scanNetworks();
    bool seen = false;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == ssid_) { seen = true; break; }
    }
    Serial.printf("scan: %d APs, SSID \"%s\" %s\n", n, ssid_, seen ? "VISIBLE" : "NOT FOUND");
    return false;
}

void TelemetryClient::maintainWifi() {
    if (wifiOk_) {
        // ESP32 STA briefly reports WL_DISCONNECTED during beacon misses / DTIM
        // wakeups even on a healthy link. Tearing the WS down on the first miss
        // caused the dashboard to flap "cluster offline". Only declare a real
        // drop after the link has been down continuously for a grace window;
        // auto-reconnect usually restores it well before that.
        if (WiFi.status() != WL_CONNECTED) {
            uint32_t now = millis();
            if (wifiDownSinceMs_ == 0) wifiDownSinceMs_ = now;
            if (now - wifiDownSinceMs_ > WIFI_DROP_GRACE_MS) {
                wifiOk_ = false;
                ip_[0] = '\0';
                wsStarted_ = false;
                wifiDownSinceMs_ = 0;
                ws.disconnect();
                Serial.println("WiFi dropped");
            }
        } else {
            wifiDownSinceMs_ = 0;
        }
        return;
    }
    if (!ssid_[0]) return;
    uint32_t now = millis();
    if (!wifiConnecting_ && now - lastWifiTryMs_ > 10000) {
        lastWifiTryMs_ = now;
        wifiConnecting_ = true;
        wifiConnectStartMs_ = now;
        WiFi.begin(ssid_, pass_);
        Serial.println("WiFi retry...");
    }
    if (!wifiConnecting_) return;
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnecting_ = false;
        wifiOk_ = true;
        strncpy(ip_, WiFi.localIP().toString().c_str(), sizeof(ip_) - 1);
        Serial.printf("WiFi OK (%s) RSSI=%d\n", ip_, WiFi.RSSI());
        startWs();
        return;
    }
    if (now - wifiConnectStartMs_ > 20000) {
        wifiConnecting_ = false;
        Serial.printf("WiFi retry timed out (%s)\n", wifiStatusName(WiFi.status()));
    }
}

bool TelemetryClient::begin(const char* ssid, const char* pass) {
    g_self = this;
    if (!frame_) frame_ = (uint8_t*)malloc(4 + DS_PLANE * NCH);   // heap: keep static DRAM small

    // Durable spool comes up regardless of WiFi (headless autonomy still records).
    if (LittleFS.begin(true)) {
        evLog_.begin(LittleFS, "/ev", false);    // events: precious, never evicted
        vitLog_.begin(LittleFS, "/vit", true);   // vitals: evictable under pressure
    }

    strncpy(ssid_, ssid ? ssid : "", sizeof(ssid_) - 1);
    strncpy(pass_, pass ? pass : "", sizeof(pass_) - 1);
    return connectWifi(45000);   // patient connect (mining firmware waited forever)
}

void TelemetryClient::loop() {
    pumpWs();
    maintainWifi();
    if (wifiOk_ && !wsStarted_) startWs();
    // T2b vitals snapshot sampler (durable), decoupled from the field cadence.
    if (vitals_ && millis() - lastSnapMs_ > VITALS_SNAP_MS) {
        lastSnapMs_ = millis();
        const WorldVitals& v = *vitals_;
        VitalsSnap s;
        s.gen = v.generation; s.mass = v.worldMass; s.activity = v.worldActivity;
        s.entropy = v.worldEntropy; s.bestFitness = v.bestFitness; s.bestStrip = (int16_t)v.bestStrip;
        s.coupling = v.coupling; s.organismsAlive = (uint16_t)v.organismsAlive;
        s.births = v.births; s.deaths = v.deaths; s.migrations = v.migrations;
        s.seamCrossings = v.seamCrossings; s.online = (uint8_t)v.online;
        uint32_t vseq = vitLog_.append((const uint8_t*)&s, sizeof(s));
        sendSnapJson(vseq, s);
    }
}

bool TelemetryClient::connected() const { return ws.isConnected(); }

void TelemetryClient::broadcastField(const Stitch& stitch) {
    if (!ws.isConnected() || !frame_) return;
    const int nch = stitch.channels();
    const int bytes = DS_PLANE * nch;
    frame_[0] = 0xCA;
    frame_[1] = (uint8_t)stitch.width();
    frame_[2] = (uint8_t)stitch.height();
    frame_[3] = (uint8_t)nch;                 // species count (new field)
    memcpy(frame_ + 4, stitch.fieldMulti(), bytes);
    ws.sendBIN(frame_, 4 + bytes);
    pumpWs();
}

void TelemetryClient::broadcastVitals(const WorldVitals& v) {
    if (!ws.isConnected()) return;
    StaticJsonDocument<1024> d;
    d["t"] = "vitals";
    d["gen"] = v.generation;
    d["online"] = v.online;
    d["mass"] = v.worldMass;
    d["mass1"] = v.worldMass1;      // species-1 share -> dashboard species split
    d["activity"] = v.worldActivity;
    d["entropy"] = v.worldEntropy;
    d["bestFitness"] = v.bestFitness;
    d["bestStrip"] = v.bestStrip;
    d["coupling"] = v.coupling;
    d["organisms"] = v.organismsAlive;
    d["births"] = v.births;
    d["deaths"] = v.deaths;
    d["migrations"] = v.migrations;
    d["seamCrossings"] = v.seamCrossings;
    if (stats_) {
        JsonArray strips = d.createNestedArray("strips");
        for (int i = 0; i < N_STRIPS; i++) {
            JsonObject row = strips.createNestedObject();
            row["strip"] = i;
            row["bank"] = stripBank(i);
            row["fitness"] = stats_[i].fitness;
        }
    }
    String s; serializeJson(d, s); ws.sendTXT(s);
    pumpWs();
}

void TelemetryClient::pushEvent(const char* type, const char* text) {
    // Lightweight log/narrator feed (not the durable fossil stream, which flows
    // through onFossil()). Sent best-effort so the dashboard's event ticker is live.
    if (!ws.isConnected()) return;
    StaticJsonDocument<256> d;
    d["t"] = "log";
    d["type"] = type;
    d["text"] = text;
    String s; serializeJson(d, s); ws.sendTXT(s);
}

void TelemetryClient::registerSink(Lineage& lineage) {
    lineage.setSink(lineageSinkTrampoline, this);
}

void TelemetryClient::onFossil(const LineageEvent& e) {
    uint32_t eseq = evLog_.append((const uint8_t*)&e, sizeof(e));   // durable first
    sendEventJson(eseq, e);                                          // then live (iff connected)
}

}  // namespace chimera
