#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

#include "webserver.h"
#include "topology.h"
#include "index_html.h"

namespace chimera {

static WebServer http(80);
static WebSocketsServer ws(81);

// Set each loop() so the static HTTP handlers can reach the latest state.
static const WorldVitals* s_vitals = nullptr;
static const StripStats*  s_stats = nullptr;

static void handleRoot() { http.send_P(200, "text/html", INDEX_HTML); }

static void handleApi() {
    StaticJsonDocument<2048> doc;
    if (s_vitals) {
        const WorldVitals& v = *s_vitals;
        doc["generation"] = v.generation;
        doc["online"] = v.online;
        doc["mass"] = v.worldMass;
        doc["entropy"] = v.worldEntropy;
        doc["bestFitness"] = v.bestFitness;
        doc["bestStrip"] = v.bestStrip;
        doc["coupling"] = v.coupling;
        doc["organisms"] = v.organismsAlive;
        doc["births"] = v.births;
        doc["deaths"] = v.deaths;
        doc["migrations"] = v.migrations;
        doc["seamCrossings"] = v.seamCrossings;
    }
    JsonArray arr = doc.createNestedArray("strips");
    if (s_stats) {
        for (int i = 0; i < N_STRIPS; i++) {
            JsonObject o = arr.createNestedObject();
            o["strip"] = i;
            o["bank"] = s_stats[i].bank;
            o["mass"] = s_stats[i].mass;
            o["activity"] = s_stats[i].activity;
            o["entropy"] = s_stats[i].entropy;
            o["fitness"] = s_stats[i].fitness;
        }
    }
    String out;
    serializeJson(doc, out);
    http.sendHeader("Access-Control-Allow-Origin", "*");
    http.send(200, "application/json", out);
}

static void onWsEvent(uint8_t, WStype_t, uint8_t*, size_t) {}

bool WebViz::begin(const char* ssid, const char* pass) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(250);
    connected_ = WiFi.status() == WL_CONNECTED;
    if (connected_) {
        strncpy(ip_, WiFi.localIP().toString().c_str(), sizeof(ip_) - 1);
        http.on("/", handleRoot);
        http.on("/api", handleApi);
        http.onNotFound([]() { http.send(404, "text/plain", "not found"); });
        http.begin();
        ws.begin();
        ws.onEvent(onWsEvent);
    }
    return connected_;
}

void WebViz::loop() {
    if (!connected_) return;
    s_vitals = vitals_;
    s_stats = stats_;
    http.handleClient();
    ws.loop();
}

void WebViz::broadcastField(const Stitch& stitch) {
    if (!connected_) return;
    frame_[0] = 0xCA;
    frame_[1] = (uint8_t)stitch.width();
    frame_[2] = (uint8_t)stitch.height();
    memcpy(frame_ + 3, stitch.field(), DS_W * DS_H);
    ws.broadcastBIN(frame_, 3 + DS_W * DS_H);
}

void WebViz::broadcastVitals(const WorldVitals& v) {
    if (!connected_) return;
    StaticJsonDocument<512> doc;
    doc["t"] = "vitals";
    doc["gen"] = v.generation;
    doc["online"] = v.online;
    doc["mass"] = v.worldMass;
    doc["entropy"] = v.worldEntropy;
    doc["bestFitness"] = v.bestFitness;
    doc["coupling"] = v.coupling;
    doc["organisms"] = v.organismsAlive;
    doc["births"] = v.births;
    doc["migrations"] = v.migrations;
    doc["seamCrossings"] = v.seamCrossings;
    String out;
    serializeJson(doc, out);
    ws.broadcastTXT(out);
}

void WebViz::pushEvent(const char* type, const char* text) {
    if (!connected_) return;
    StaticJsonDocument<256> doc;
    doc["t"] = "event";
    doc["type"] = type;
    doc["text"] = text;
    String out;
    serializeJson(doc, out);
    ws.broadcastTXT(out);
}

}  // namespace chimera
