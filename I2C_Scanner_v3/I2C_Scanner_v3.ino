/*
 * Dual-Bus I2C Scanner v3 - robust inventory
 *
 * Fixes "only 2-4 random slaves show up": the ESP32 core 3.x I2C driver
 * wedges after the FIRST address that doesn't answer cleanly, blacking out
 * every address after it in that pass. This version recovers the driver after
 * every failed probe and retries each address, so one missing/slow/colliding
 * slave can't mask the others.
 *
 * It checks the 10 EXPECTED addresses and reports PRESENT/missing per bus,
 * then does a light sweep for any UNEXPECTED responder (wrong-address or
 * address-collision clue).
 *
 * NOTE: the attention line is asserted during each bus scan to mirror the
 * mining firmware. During a scan the slaves have no job and aren't mining, so
 * this doesn't change results here - it's just kept consistent with the real
 * system. The actual fix for the random-dropouts is the per-probe recovery.
 */
#include <Wire.h>

#define I2C_0_SDA 21   // master pin 33
#define I2C_0_SCL 22   // master pin 36
#define ATTN_0_PIN 23  // master pin 37
#define I2C_1_SDA 32   // master pin 8
#define I2C_1_SCL 33   // master pin 9
#define ATTN_1_PIN 25  // master pin 10

uint8_t bus0_expected[] = {0x08, 0x09, 0x0A, 0x0B, 0x0C}; // S3s
uint8_t bus1_expected[] = {0x18, 0x19, 0x1A, 0x1B, 0x1C}; // C3s

const int RETRIES = 4;

void recover(TwoWire &bus, int sda, int scl) {
    bus.end();
    delay(3);
    bus.begin(sda, scl, 100000);
    delay(3);
}

bool probe(TwoWire &bus, int sda, int scl, uint8_t addr) {
    for (int i = 0; i < RETRIES; i++) {
        bus.beginTransmission(addr);
        if (bus.endTransmission() == 0) return true;
        recover(bus, sda, scl);   // clear any wedge this probe caused
    }
    return false;
}

void scanBus(TwoWire &bus, int sda, int scl, int attn,
             uint8_t* expected, int n, const char* name) {
    recover(bus, sda, scl);
    pinMode(attn, OUTPUT);
    digitalWrite(attn, HIGH);          // assert attention (mirrors mining flow)
    delayMicroseconds(1500);

    Serial.printf("\n%s\n", name);
    int present = 0;
    for (int i = 0; i < n; i++) {
        bool ok = probe(bus, sda, scl, expected[i]);
        Serial.printf("   0x%02X : %s\n", expected[i], ok ? "PRESENT" : "-- missing --");
        if (ok) present++;
    }

    // Light best-effort sweep for anything at an unexpected address.
    // (Single attempt each; if a wrong-address slave or collision is out there
    //  it usually shows up here. The next scanBus re-inits regardless.)
    for (uint8_t a = 1; a < 127; a++) {
        bool exp = false;
        for (int i = 0; i < n; i++) if (expected[i] == a) exp = true;
        if (exp) continue;
        bus.beginTransmission(a);
        if (bus.endTransmission() == 0) {
            Serial.printf("   !! UNEXPECTED device answering at 0x%02X\n", a);
            recover(bus, sda, scl);
        }
    }

    digitalWrite(attn, LOW);
    Serial.printf("   --> %d / %d expected present\n", present, n);
}

void setup() {
    Serial.begin(115200);
    delay(1500);   // settle after main power; let CDC enumerate
    pinMode(ATTN_0_PIN, OUTPUT); digitalWrite(ATTN_0_PIN, LOW);
    pinMode(ATTN_1_PIN, OUTPUT); digitalWrite(ATTN_1_PIN, LOW);
    Serial.println("\n=== Dual-Bus I2C Scanner v3 (per-probe recovery) ===");
}

void loop() {
    scanBus(Wire,  I2C_0_SDA, I2C_0_SCL, ATTN_0_PIN, bus0_expected, 5, "Bus 0  (S3s, GPIO 21/22)");
    scanBus(Wire1, I2C_1_SDA, I2C_1_SCL, ATTN_1_PIN, bus1_expected, 5, "Bus 1  (C3s, GPIO 32/33)");
    Serial.println("\n--- rescan in 3s ---");
    delay(3000);
}
