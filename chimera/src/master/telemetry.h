// telemetry.h - outbound telemetry client to the Selis server (replaces the
// old local WebServer/WebSocketsServer). The master dials OUT to Selis over one
// WebSocket and is never a dependency of the cluster (see
// docs/cluster-telemetry-and-persistence.md). Durable streams (fossil events +
// vitals snapshots) are spooled to flash and replayed on reconnect; the live
// field + vitals are best-effort T1 (dropped when Selis is down).
//
// Keeps broadcastField/broadcastVitals/pushEvent so the rest of the firmware is
// unchanged; the durable fossil stream is fed automatically via lineage's sink.
#pragma once

#include <stdint.h>
#include "world_state.h"
#include "stitch.h"
#include "lineage.h"
#include "persist.h"

namespace chimera {

class TelemetryClient {
public:
    bool begin(const char* ssid, const char* pass);
    void loop();
    void attach(const WorldVitals* v, const StripStats* stats) { vitals_ = v; stats_ = stats; }

    void broadcastField(const Stitch& stitch);     // T1 binary, iff connected
    void broadcastVitals(const WorldVitals& v);    // T1 JSON, iff connected
    void pushEvent(const char* type, const char* text);  // narrator/log feed (text)

    // Route every Lineage event into the durable spool + Selis.
    void registerSink(Lineage& lineage);
    // Durable fossil sink (called by the lineage trampoline).
    void onFossil(const LineageEvent& e);

    bool connected() const;
    bool wifiLinked() const { return wifiOk_; }
    const char* ip() const { return ip_; }
    void service();   // pump WebSocket + WiFi (call during long I2C work)

    LogStream& events() { return evLog_; }
    LogStream& vitals() { return vitLog_; }

private:
    const WorldVitals* vitals_ = nullptr;
    const StripStats*  stats_ = nullptr;
    char ip_[20] = {0};
    char ssid_[33] = {0};
    char pass_[65] = {0};
    bool wifiOk_ = false;
    bool wsStarted_ = false;
    uint8_t* frame_ = nullptr;   // [0xCA][w][h][nch][nch planes], heap (begin())
    uint32_t lastSnapMs_ = 0;
    uint32_t lastWifiTryMs_ = 0;
    uint32_t wifiConnectStartMs_ = 0;
    bool wifiConnecting_ = false;

    void startWs();
    bool connectWifi(uint32_t waitMs);
    void maintainWifi();

    LogStream evLog_;
    LogStream vitLog_;
};

}  // namespace chimera
