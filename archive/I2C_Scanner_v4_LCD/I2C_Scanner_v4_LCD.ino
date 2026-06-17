/*
 * Dual-Bus I2C Scanner v4 - now with ST7789 LCD status display
 *
 * Same robust per-probe recovery + retry scan as v3, but mirrors the result
 * to a 240x240 ST7789 LCD so you can read cluster health at a glance without
 * a serial monitor. Serial output is kept too.
 *
 * LCD: 1.54" / 2" ST7789 240x240, hardware SPI, Adafruit_ST7789 library.
 *   Install via Library Manager: "Adafruit ST7735 and ST7789 Library"
 *   + "Adafruit GFX Library".
 *
 * LCD wiring (master WROOM-32):
 *   VCC -> 3V3        GND -> GND
 *   SCL -> GPIO18 (clock)    SDA -> GPIO19 (data)
 *   RES -> GPIO4      DC  -> GPIO26     CS -> GPIO27
 *   BLK -> 3V3 (always on)
 */
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// ---------- I2C bus pins ----------
#define I2C_0_SDA 21   // master pin 33
#define I2C_0_SCL 22   // master pin 36
#define ATTN_0_PIN 23  // master pin 37
#define I2C_1_SDA 32   // master pin 8
#define I2C_1_SCL 33   // master pin 9
#define ATTN_1_PIN 25  // master pin 10

// ---------- LCD pins ----------
#define TFT_CS    27
#define TFT_DC    26
#define TFT_RST    4
#define TFT_MOSI  19   // LCD "SDA"
#define TFT_SCLK  18   // LCD "SCL"
// Software SPI on the EXACT pins you wired. We can't use the ESP32's default
// hardware-SPI data pin (MOSI = GPIO 23) because GPIO 23 is your Bus-0
// attention line. The 5-arg constructor bit-bangs on the given pins, so data
// always goes out on GPIO 19 regardless of the SPI peripheral's defaults.
// (The blank-but-backlit screen was the library clocking data on GPIO 23 -
//  unconnected - while the panel's SDA sat on GPIO 19.)
// Speed penalty vs hardware SPI is irrelevant for a status screen.
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// ---------- expected slaves ----------
uint8_t bus0_expected[] = {0x08, 0x09, 0x0A, 0x0B, 0x0C}; // S3s
uint8_t bus1_expected[] = {0x18, 0x19, 0x1A, 0x1B, 0x1C}; // C3s
bool bus0_present[5] = {false,false,false,false,false};
bool bus1_present[5] = {false,false,false,false,false};

const int RETRIES = 4;

// ST7789 color shorthands (RGB565)
#define C_BG     0x0000  // black
#define C_TITLE  0xFFFF  // white
#define C_OK     0x07E0  // green
#define C_MISS   0xF800  // red
#define C_DIM    0x7BEF  // grey
#define C_HDR    0x05FF  // cyan

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
        recover(bus, sda, scl);
    }
    return false;
}

int scanBus(TwoWire &bus, int sda, int scl, int attn,
            uint8_t* expected, bool* present, int n, const char* name) {
    recover(bus, sda, scl);
    pinMode(attn, OUTPUT);
    digitalWrite(attn, HIGH);
    delayMicroseconds(1500);

    Serial.printf("\n%s\n", name);
    int found = 0;
    for (int i = 0; i < n; i++) {
        present[i] = probe(bus, sda, scl, expected[i]);
        Serial.printf("   0x%02X : %s\n", expected[i], present[i] ? "PRESENT" : "-- missing --");
        if (present[i]) found++;
    }
    digitalWrite(attn, LOW);
    Serial.printf("   --> %d / %d present\n", found, n);
    return found;
}

void drawColumn(int x, const char* header, uint8_t* expected, bool* present, int n, int found) {
    tft.setTextSize(2);
    tft.setCursor(x, 40);
    tft.setTextColor(C_HDR, C_BG);
    tft.print(header);

    // count badge
    tft.setCursor(x, 62);
    tft.setTextColor(found == n ? C_OK : (found == 0 ? C_MISS : C_DIM), C_BG);
    tft.printf("%d/%d", found, n);

    tft.setTextSize(2);
    for (int i = 0; i < n; i++) {
        int y = 92 + i * 26;
        tft.setCursor(x, y);
        tft.setTextColor(present[i] ? C_OK : C_MISS, C_BG);
        // 0xNN  + tick/cross
        tft.printf("%02X %c", expected[i], present[i] ? '+' : 'x');
    }
}

void drawScreen(int found0, int found1) {
    tft.fillScreen(C_BG);

    // title bar
    tft.setTextSize(2);
    tft.setCursor(6, 8);
    tft.setTextColor(C_TITLE, C_BG);
    tft.print("CLUSTER SCAN");

    int total = found0 + found1;
    tft.setTextSize(1);
    tft.setCursor(6, 28);
    tft.setTextColor(total == 10 ? C_OK : C_DIM, C_BG);
    tft.printf("%d / 10 slaves online", total);

    // two columns: bus 0 (S3) left, bus 1 (C3) right
    drawColumn(10,  "S3", bus0_expected, bus0_present, 5, found0);
    drawColumn(130, "C3", bus1_expected, bus1_present, 5, found1);

    // footer
    tft.drawFastHLine(0, 230, 240, C_DIM);
}

void setup() {
    Serial.begin(115200);
    delay(1500);
    pinMode(ATTN_0_PIN, OUTPUT); digitalWrite(ATTN_0_PIN, LOW);
    pinMode(ATTN_1_PIN, OUTPUT); digitalWrite(ATTN_1_PIN, LOW);

    Serial.println("\n=== Dual-Bus I2C Scanner v4 (LCD) ===");

    // ST7789 240x240 init
    tft.init(240, 240);
    tft.setRotation(0);   // try 0/1/2/3 if orientation is off
    tft.fillScreen(C_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_TITLE, C_BG);
    tft.setCursor(20, 110);
    tft.print("Scanner boot");
    delay(600);
}

void loop() {
    int found0 = scanBus(Wire,  I2C_0_SDA, I2C_0_SCL, ATTN_0_PIN,
                         bus0_expected, bus0_present, 5, "Bus 0 (S3s, GPIO 21/22)");
    int found1 = scanBus(Wire1, I2C_1_SDA, I2C_1_SCL, ATTN_1_PIN,
                         bus1_expected, bus1_present, 5, "Bus 1 (C3s, GPIO 32/33)");

    drawScreen(found0, found1);

    Serial.println("\n--- rescan in 3s ---");
    delay(3000);
}
