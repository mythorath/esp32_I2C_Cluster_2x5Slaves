#include <Arduino.h>
#include <ctype.h>
#include "narrator.h"
#include "topology.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef NARRATOR_ENABLED
#define NARRATOR_ENABLED 0
#endif

#if NARRATOR_ENABLED
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#endif

namespace chimera {

void Narrator::begin(TelemetryClient* web) {
    web_ = web;
    seed_ ^= millis();
}

static const char* MOODS[] = {"dreaming of stripes", "growing quiet", "blooming",
                              "fracturing into gliders", "holding its breath",
                              "remembering an old shape", "drifting toward chaos"};

void Narrator::compose(char* out, int cap, const WorldVitals& v, const Detector& detector) {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    const char* moodA = MOODS[(seed_ >> 3) % 7];
    const char* moodB = MOODS[(seed_ >> 9) % 7];
    const Organism* o = detector.newest();
    if (o && o->name[0] && (v.generation - o->birthGen) < 80) {
        snprintf(out, cap, "Gen %lu: a new creature, %s, has appeared in Bank %c. "
                           "Bank A is %s tonight; Bank B is %s. (%lu alive, %lu seam crossings)",
                 (unsigned long)v.generation, o->name, o->bank == BANK_A ? 'A' : 'B',
                 moodA, moodB, (unsigned long)v.organismsAlive, (unsigned long)v.seamCrossings);
    } else {
        snprintf(out, cap, "Gen %lu: Bank A is %s; Bank B is %s. "
                           "%lu creatures swim the torus; %lu have crossed the seam between worlds.",
                 (unsigned long)v.generation, moodA, moodB,
                 (unsigned long)v.organismsAlive, (unsigned long)v.migrations);
    }
}

void Narrator::update(const WorldVitals& v, const Detector& detector, uint32_t gen) {
    if (gen - lastWebGen_ >= WEB_EVERY_GEN) {
        lastWebGen_ = gen;
        char text[200];
        compose(text, sizeof text, v, detector);
        if (web_) web_->pushEvent("narrate", text);
        Serial.printf("  [narrator] %s\n", text);

        if (NARRATOR_ENABLED && millis() - lastPostMs_ > POST_EVERY_MS) {
            lastPostMs_ = millis();
            post(text);
        }
    }
}

void Narrator::post(const char* text) {
#if NARRATOR_ENABLED
    WiFiClientSecure client;
    client.setInsecure();   // skip cert pinning for hobby use
    HTTPClient https;
    String url = String("https://") + MASTODON_HOST + "/api/v1/statuses";
    if (!https.begin(client, url)) return;
    https.addHeader("Authorization", String("Bearer ") + MASTODON_TOKEN);
    https.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String body = "status=";
    for (const char* p = text; *p; p++) {       // minimal URL-encode
        char ch = *p;
        if (isalnum((int)ch)) body += ch;
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", (unsigned char)ch); body += b; }
    }
    int code = https.POST(body);
    Serial.printf("  [narrator] mastodon POST -> %d\n", code);
    https.end();
#else
    (void)text;
#endif
}

}  // namespace chimera
