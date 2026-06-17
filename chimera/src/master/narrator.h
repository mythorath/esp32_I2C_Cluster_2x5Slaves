// narrator.h - autonomous social narrator (Phase 8).
//
// Composes short narration from real world state - births, extinctions, seam
// crossings, which hemisphere is "dreaming" tonight - and posts it: always to
// the web event log, and (if configured in secrets.h) to Mastodon. Proves the
// piece narrates its own life. Heavier text generation can be offloaded to a
// NAS endpoint (NARRATOR_LLM_URL) later.
#pragma once

#include <stdint.h>
#include "world_state.h"
#include "detect.h"
#include "webserver.h"

namespace chimera {

class Narrator {
public:
    void begin(WebViz* web);
    void update(const WorldVitals& v, const Detector& detector, uint32_t gen);

private:
    void compose(char* out, int cap, const WorldVitals& v, const Detector& detector);
    void post(const char* text);

    WebViz* web_ = nullptr;
    uint32_t lastWebGen_ = 0;
    uint32_t lastPostMs_ = 0;
    uint32_t seed_ = 0xBADC0DE;

    static constexpr uint32_t WEB_EVERY_GEN = 40;
    static constexpr uint32_t POST_EVERY_MS = 300000;  // 5 min between toots
};

}  // namespace chimera
