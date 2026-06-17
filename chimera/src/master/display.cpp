#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "display.h"
#include "topology.h"

namespace chimera {

// Software-SPI on the exact wired pins (HW SPI MOSI default GPIO23 is the bus-0
// attention line, so it must be bit-banged on GPIO19 - see archived notes).
#define TFT_CS 27
#define TFT_DC 26
#define TFT_RST 4
#define TFT_MOSI 19
#define TFT_SCLK 18
static Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

#define C_BG     0x0000
#define C_CYAN   0x07FF
#define C_MAG    0xF81F
#define C_GREEN  0x07E0
#define C_AMBER  0xFD20
#define C_GREY   0x52AA
#define C_WHITE  0xFFFF
#define C_RED    0xF800

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void Display::begin() {
    tft.init(240, 240);
    tft.setRotation(0);
    tft.fillScreen(C_BG);
}

void Display::boot(const char* msg) {
    tft.fillScreen(C_BG);
    tft.setTextColor(C_CYAN, C_BG);
    tft.setTextSize(2);
    tft.setCursor(20, 90);
    tft.print("CHIMERA LENIA");
    tft.setTextSize(1);
    tft.setTextColor(C_GREY, C_BG);
    tft.setCursor(20, 120);
    tft.print(msg);
}

// Bank A (C3/fixed): black->teal->green->white. Bank B (S3/float): black->
// purple->magenta->amber. Different palettes make the seam pop.
uint16_t Display::colorFor(uint8_t u, int bank) {
    float t = u / 255.0f;
    if (bank == BANK_A) {
        uint8_t r = (uint8_t)(t * t * 255);
        uint8_t g = (uint8_t)(t * 255);
        uint8_t b = (uint8_t)((1.0f - t) * 120 * t * 2);
        return rgb565(r, g, b);
    } else {
        uint8_t r = (uint8_t)(t * 255);
        uint8_t g = (uint8_t)(t * t * 180);
        uint8_t b = (uint8_t)((1.0f - t * 0.5f) * 200 * t);
        return rgb565(r, g, b);
    }
}

void Display::chrome() {
    tft.fillScreen(C_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_CYAN, C_BG);
    tft.setCursor(6, 6);
    tft.print("CHIMERA");
    tft.setTextSize(1);
    tft.setTextColor(C_GREY, C_BG);
    tft.setCursor(120, 6);
    tft.print("LENIA");
    tft.setCursor(120, 16);
    tft.print("ALife");
    tft.drawFastHLine(0, 34, 240, C_GREY);
    chromeDrawn_ = true;
}

void Display::renderField(const Stitch& stitch) {
    if (!chromeDrawn_) chrome();
    const int w = stitch.width();   // 64
    const int h = stitch.height();  // 200
    static uint16_t line[WORLD_W];  // 128 px (2 per DS col)
    tft.startWrite();
    tft.setAddrWindow(FX, FY, w * FPX, h);
    for (int r = 0; r < h; r++) {
        int strip = Stitch::stripOfRow(r);
        int bank = (strip < NODES_PER_BANK) ? BANK_A : BANK_B;
        for (int c = 0; c < w; c++) {
            uint16_t col = colorFor(stitch.at(r, c), bank);
            line[c * FPX] = col;
            line[c * FPX + 1] = col;
        }
        tft.writePixels(line, w * FPX);
    }
    tft.endWrite();
    // seam markers (between strip 4/5 and at the wrap top/bottom)
    int seamY = FY + NODES_PER_BANK * DS_STRIP_H;
    tft.drawFastHLine(FX, seamY, w * FPX, C_AMBER);
}

void Display::renderVitals(const WorldVitals& v, const Organism* newest) {
    if (!chromeDrawn_) chrome();
    const int px = FX + WORLD_W + 6;   // right panel x
    char buf[28];
    tft.setTextSize(1);

    auto kv = [&](int y, uint16_t c, const char* fmt, float val, bool isInt) {
        tft.fillRect(px, y, 240 - px, 9, C_BG);
        if (isInt) snprintf(buf, sizeof buf, fmt, (long)val);
        else snprintf(buf, sizeof buf, fmt, val);
        tft.setTextColor(c, C_BG);
        tft.setCursor(px, y);
        tft.print(buf);
    };

    // network: IP (or headless) - prominent at the top of the panel
    tft.fillRect(px, 40, 240 - px, 9, C_BG);
    tft.setTextWrap(false);
    tft.setTextColor(v.wifiOk ? C_GREEN : C_GREY, C_BG);
    tft.setCursor(px, 40);
    tft.print(v.wifiOk ? v.ip : "headless");

    kv(54, C_WHITE, "gen %ld", (float)v.generation, true);
    kv(66, C_CYAN, "mass %.2f", v.worldMass, false);
    kv(78, C_CYAN, "ent  %.2f", v.worldEntropy, false);
    kv(90, C_GREEN, "fit  %.2f", v.bestFitness, false);
    kv(102, C_MAG, "cpl  %.2f", v.coupling, false);
    // orgs / births / migrations compact on one line
    tft.fillRect(px, 114, 240 - px, 9, C_BG);
    tft.setTextColor(C_AMBER, C_BG);
    tft.setCursor(px, 114);
    snprintf(buf, sizeof buf, "o%lu b%lu m%lu", (unsigned long)v.organismsAlive,
             (unsigned long)v.births, (unsigned long)v.migrations);
    tft.print(buf);

    // node-status grid: A0..A4 (left col) + B0..B4 (right col), green=online
    tft.fillRect(px, 126, 240 - px, 9, C_BG);
    tft.setTextColor(v.online == N_STRIPS ? C_GREEN : C_AMBER, C_BG);
    tft.setCursor(px, 126);
    snprintf(buf, sizeof buf, "NODES %d/10", v.online);
    tft.print(buf);
    for (int i = 0; i < N_STRIPS; i++) {
        int col = (i < NODES_PER_BANK) ? 0 : 1;
        int rowN = i % NODES_PER_BANK;
        int x = px + col * 48;
        int y = 140 + rowN * 14;
        uint16_t c = v.nodeOnline[i] ? C_GREEN : C_RED;
        tft.fillRect(x, y, 46, 12, C_BG);
        tft.fillCircle(x + 4, y + 5, 3, c);
        tft.setTextColor(c, C_BG);
        tft.setCursor(x + 11, y + 1);
        snprintf(buf, sizeof buf, "%c%d", (i < NODES_PER_BANK) ? 'A' : 'B', rowN);
        tft.print(buf);
    }

    // newest organism card (bottom)
    tft.fillRect(0, 222, 240, 18, C_BG);
    tft.drawFastHLine(0, 220, 240, C_GREY);
    if (newest && newest->name[0]) {
        tft.setTextColor(newest->bank == BANK_A ? C_GREEN : C_MAG, C_BG);
        tft.setCursor(6, 226);
        snprintf(buf, sizeof buf, "%s", newest->name);
        tft.print(buf);
        tft.setTextColor(C_GREY, C_BG);
        snprintf(buf, sizeof buf, "%c age %lu", newest->bank == BANK_A ? 'A' : 'B',
                 (unsigned long)(v.generation - newest->birthGen));
        tft.setCursor(140, 226);
        tft.print(buf);
    }
}

}  // namespace chimera
