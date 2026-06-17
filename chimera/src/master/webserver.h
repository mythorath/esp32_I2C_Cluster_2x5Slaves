// webserver.h - WiFi dashboard + WebSocket field stream for the master.
//
// HTTP (port 80) serves the canvas visualizer; /api returns JSON vitals.
// WebSocket (port 81) streams the stitched downsampled field as a compact
// binary frame [0xCA][w][h][w*h bytes], plus interleaved JSON vitals as text,
// so the browser renders the "Instagram money shot" with Bank A and Bank B
// colored differently and the seam + migrations visible.
#pragma once

#include <stdint.h>
#include "world_state.h"
#include "stitch.h"

namespace chimera {

class WebViz {
public:
    bool begin(const char* ssid, const char* pass);
    void loop();
    void attach(const WorldVitals* v, const StripStats* stats) { vitals_ = v; stats_ = stats; }

    void broadcastField(const Stitch& stitch);
    void broadcastVitals(const WorldVitals& v);
    void pushEvent(const char* type, const char* text);  // narrator/LED feed

    bool connected() const { return connected_; }
    const char* ip() const { return ip_; }

private:
    const WorldVitals* vitals_ = nullptr;
    const StripStats*  stats_ = nullptr;
    bool connected_ = false;
    char ip_[20] = {0};
    uint8_t frame_[3 + DS_W * DS_H];
};

}  // namespace chimera
