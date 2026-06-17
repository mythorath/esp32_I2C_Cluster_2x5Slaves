#include "led_events.h"

#if LED_ENABLED
#include <Adafruit_NeoPixel.h>
static Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
#endif

namespace chimera {

void LedEvents::begin() {
#if LED_ENABLED
    strip.begin();
    strip.clear();
    strip.show();
#endif
}

void LedEvents::pulse(LedEvent e, uint8_t r, uint8_t g, uint8_t b) {
#if LED_ENABLED
    // Minimal flash; replace with a per-event running pulse along the strip.
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(r, g, b));
    strip.show();
#else
    (void)e; (void)r; (void)g; (void)b;
#endif
}

void LedEvents::update() {
#if LED_ENABLED
    // Fade toward black so pulses decay between events.
    for (int i = 0; i < LED_COUNT; i++) {
        uint32_t c = strip.getPixelColor(i);
        uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        strip.setPixelColor(i, strip.Color(r * 7 / 8, g * 7 / 8, b * 7 / 8));
    }
    strip.show();
#endif
}

void LedEvents::onSeamCrossing(uint32_t n) {
    seam_ += n;
    pulse(LED_SEAM, 245, 166, 35);   // amber = the seam between number systems
}

void LedEvents::onBirth(uint8_t bank, float, float) {
    births_++;
    if (bank == 0) pulse(LED_BIRTH, 34, 224, 160);   // green = Bank A
    else           pulse(LED_BIRTH, 224, 86, 200);   // magenta = Bank B
}

void LedEvents::onDeath(uint8_t) { deaths_++; pulse(LED_DEATH, 120, 0, 0); }

void LedEvents::onMigration(int, int) { migr_++; pulse(LED_MIGRATION, 34, 211, 238); }

void LedEvents::onCouplingChange(float) { pulse(LED_COUPLING, 80, 80, 200); }

}  // namespace chimera
