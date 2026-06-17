// led_events.h - transmission-line LED event hub (Phase 8 show layer).
//
// Addressable LEDs along the copper runs pulse on REAL internal events: halo
// crossing the seam, a migration, an organism birth/death, a coupling change.
// This makes the sculpture's "thinking" physically visible.
//
// Hardware is optional/aspirational: enable with -DLED_ENABLED=1 and wire a
// WS2812 strip to LED_PIN (then add adafruit/Adafruit NeoPixel to lib_deps and
// fill in the pulse() rendering). Until then this logs events to serial and
// keeps the event hooks stable so the rest of the system can drive it.
#pragma once

#include <Arduino.h>

#ifndef LED_ENABLED
#define LED_ENABLED 0
#endif
#ifndef LED_PIN
#define LED_PIN 13
#endif
#ifndef LED_COUNT
#define LED_COUNT 30
#endif

namespace chimera {

enum LedEvent : uint8_t { LED_HALO, LED_SEAM, LED_BIRTH, LED_DEATH, LED_MIGRATION, LED_COUPLING };

class LedEvents {
public:
    void begin();
    void update();                       // advance/fade pulses (call off hot path)

    void onSeamCrossing(uint32_t n);
    void onBirth(uint8_t bank, float x, float y);
    void onDeath(uint8_t bank);
    void onMigration(int fromStrip, int toStrip);
    void onCouplingChange(float coupling);

private:
    void pulse(LedEvent e, uint8_t r, uint8_t g, uint8_t b);
    uint32_t seam_ = 0, births_ = 0, deaths_ = 0, migr_ = 0;
};

}  // namespace chimera
