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

// ----- helpers to emit the JSON envelopes (shared by live + replay) -----
static void sendEventJson(uint32_t eseq, const LineageEvent& e) {
    if (!ws.isConnected()) return;
    StaticJsonDocument<256> d;
    d["t"] = "event";
    d["eseq"] = eseq;
    d["gen"] = e.gen;
    d["kind"] = Lineage::typeName(e.type);
    d["fromStrip"] = e.fromStrip;
    d["toStrip"] = e.toStrip;
    d["lineageId"] = e.lineageId;
    d["organismId"] = e.organismId;
    d["fitness"] = e.fitness;
    String s; serializeJson(d, s); ws.sendTXT(s);
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
    LineageEvent e; memcpy(&e, payload, sizeof(e));
    sendEventJson(seq, e);
}
static void replaySnapCb(uint32_t seq, const uint8_t* payload, uint8_t len, void*) {
    if (len < sizeof(VitalsSnap)) return;
    VitalsSnap v; memcpy(&v, payload, sizeof(v));
    sendSnapJson(seq, v);
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
    StaticJsonDocument<256> d;
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
    }
}

static void onWsEvent(WStype_t type, uint8_t* payload, size_t len) {
    switch (type) {
        case WStype_CONNECTED: sendHello(); break;
        case WStype_TEXT:      onWsText(payload, len); break;
        default: break;
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

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(250);
    wifiOk_ = WiFi.status() == WL_CONNECTED;
    if (wifiOk_) {
        strncpy(ip_, WiFi.localIP().toString().c_str(), sizeof(ip_) - 1);
        // Resolve selis.local via mDNS (DHCP-proof); else use the configured host.
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
    }
    return wifiOk_;
}

void TelemetryClient::loop() {
    ws.loop();
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
}

void TelemetryClient::broadcastVitals(const WorldVitals& v) {
    if (!ws.isConnected()) return;
    StaticJsonDocument<512> d;
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
    String s; serializeJson(d, s); ws.sendTXT(s);
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
