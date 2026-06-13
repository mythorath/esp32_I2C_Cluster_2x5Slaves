/*
 * SHA256 Mining Cluster Master for ESP32
 * Dual I2C Bus Management (10 Slaves)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include "mbedtls/sha256.h"

// Private credentials live in secrets.h (git-ignored).
// Copy secrets.example.h -> secrets.h and fill in your own values.
#include "secrets.h"

// ============== I2C & PIN CONFIGURATION ==============
// Bus 0 (The 5x S3 Slaves)
#define I2C_0_SDA 21 // Pin D33 on your board
#define I2C_0_SCL 22 // Pin D36 on your board
#define ATTN_0_PIN 23 // Pin D37 on your board

// Bus 1 (The 5x C3 Slaves)
#define I2C_1_SDA 32 // Capable I/O pin
#define I2C_1_SCL 33 // Capable I/O pin
#define ATTN_1_PIN 25 // Capable I/O pin

// ============== LCD (ST7789 240x240, SOFTWARE SPI) ==============
// Wiring (verified by I2C_Scanner_v4_LCD):
//   VCC->3V3  GND->GND  SCL->GPIO18  SDA->GPIO19
//   RES->GPIO4  DC->GPIO26  CS->GPIO27  BLK->3V3
//
// MUST use the 5-arg software-SPI constructor. Hardware SPI's default MOSI is
// GPIO23 -- which is our ATTN_0_PIN -- so the panel's SDA on GPIO19 never gets
// data (backlit but black). Bit-banging on the exact wired pins fixes that.
#define TFT_CS    27
#define TFT_DC    26
#define TFT_RST    4
#define TFT_MOSI  19   // LCD "SDA"
#define TFT_SCLK  18   // LCD "SCL"
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// Cyberpunk palette (RGB565)
#define UI_BG     0x0000  // black
#define UI_DARK   0x10A2  // very dark blue-grey (frame fills)
#define UI_CYAN   0x07FF  // neon cyan
#define UI_MAGENTA 0xF81F // neon magenta
#define UI_GREEN  0x07E0  // neon green
#define UI_YELLOW 0xFFE0  // neon yellow
#define UI_RED    0xF800  // red (offline / reject)
#define UI_GREY   0x52AA  // dim grey
#define UI_WHITE  0xFFFF
#define UI_HASH   0x0438  // dim teal for idle digit churn
#define UI_HASHHI 0x07FF  // bright cyan for freshly-changed digit
#define UI_ACCENT 0xFD20  // amber accent

// ============== PROTOCOL CONSTANTS ==============
#define CMD_NEW_JOB     0x01
#define CMD_STOP        0x02
#define CMD_STATUS      0x03
#define CMD_SET_RANGE   0x04

#define RESP_READY      0x00
#define RESP_BUSY       0x01
#define RESP_FOUND      0x02
#define RESP_EXHAUSTED  0x03

// ============== DEFAULT CONFIGURATION ==============
struct Config {
    char ssid[32];
    char password[64];
    char poolHost[64];
    uint16_t poolPort;
    char btcAddress[64];
    char workerName[32];
    char workerPass[32];
    uint32_t magic;
};

// NOTE: Default credentials below are placeholders. Set your real values in
// secrets.h (see secrets.example.h) which is git-ignored, or override at runtime.
Config config = {
    WIFI_SSID,                         
    WIFI_PASSWORD,                     
    POOL_HOST,                         
    POOL_PORT,       
    BTC_ADDRESS,           
    WORKER_NAME,                       
    WORKER_PASS,                       
    0                                  
};

// ============== STATE ==============
struct StratumJob {
    String jobId;
    String prevHash;
    String coinbase1;
    String coinbase2;
    String merkleRoot;
    String version;
    String nbits;
    String ntime;
    String extraNonce2;
    bool cleanJobs;
    uint32_t jobIdNum;
    uint8_t target[32];
    uint8_t header[80];
    bool valid;
};

struct SlaveState {
    uint8_t address;
    uint8_t bus; // 0 or 1
    uint8_t status;
    uint32_t hashrate;
    uint32_t smoothedHashrate;
    uint32_t nonceStart;
    uint32_t nonceEnd;
    uint32_t lastJobId;
    unsigned long lastPoll;
    bool online;
    uint32_t lastSubmittedNonce;
    uint32_t lastSubmittedJobId;
};

// 10 Slaves: 5x S3s on Bus 0, 5x C3s on Bus 1
SlaveState slaves[10] = {
    {0x08, 0, 0, 0, 0, 0, 0, 0, 0, false, 0, 0}, 
    {0x09, 0, 0, 0, 0, 0, 0, 0, 0, false, 0, 0}, 
    {0x0A, 0, 0, 0, 0, 0, 0, 0, 0, false, 0, 0}, 
    {0x0B, 0, 0, 0, 0, 0, 0, 0, 0, false, 0, 0}, 
    {0x0C, 0, 0, 0, 0, 0, 0, 0, 0, false, 0, 0},
    {0x18, 1, 0, 0, 0, 0, 0, 0, 0, false, 0, 0}, 
    {0x19, 1, 0, 0, 0, 0, 0, 0, 0, false, 0, 0}, 
    {0x1A, 1, 0, 0, 0, 0, 0, 0, 0, false, 0, 0}, 
    {0x1B, 1, 0, 0, 0, 0, 0, 0, 0, false, 0, 0}, 
    {0x1C, 1, 0, 0, 0, 0, 0, 0, 0, false, 0, 0}
};

StratumJob currentJob;
String extraNonce1 = "";
int extraNonce2Size = 4;
uint32_t extraNonce2Counter = 0;
double poolDifficulty = 0.00005;

uint32_t totalHashrate = 0;
uint32_t sharesFound = 0;
uint32_t sharesAccepted = 0;
uint32_t sharesRejected = 0;
uint8_t shareTarget[32];

uint32_t stratumId = 1;
uint32_t subscribeId = 0;
uint32_t authorizeId = 0;
std::vector<uint32_t> submitIds;

WiFiClient stratumClient;
WebServer webServer(80);

// ============== DISPLAY STATE ==============
enum DisplayView { VIEW_MAIN, VIEW_NODES };
DisplayView currentView = VIEW_MAIN;
bool viewJustEntered = true;        // force a full static redraw on the next pass
unsigned long viewEnteredAt = 0;    // millis the current view became active
const unsigned long MAIN_VIEW_MS  = 12000;  // dwell time on the main dashboard
const unsigned long NODES_VIEW_MS = 5000;   // dwell time on the node grid

// Rolling SHA-256 digest "churn" animation buffer (64 hex chars).
char hashDigest[65];
unsigned long lastChurn = 0;
const unsigned long CHURN_MS = 80;          // how often a few digits scramble

// Cached values so we only repaint stats that actually changed (no flicker).
uint32_t shownHashrate = 0xFFFFFFFF;
uint32_t shownFound = 0xFFFFFFFF, shownAccepted = 0xFFFFFFFF, shownRejected = 0xFFFFFFFF;
int shownOnline = -1;
uint8_t shownNodeStatus[10];        // last-drawn online flag (0/1/0xFF=unset) per node
uint32_t shownNodeRate[10];         // last-drawn per-node hashrate

// Share-found celebration.
uint8_t lastFoundHash[32] = {0};    // raw hash of the most recent winning share
bool haveFoundHash = false;
uint32_t lastCelebratedShares = 0;  // sharesFound value already celebrated
bool celebrating = false;
unsigned long celebrationStart = 0;
const unsigned long CELEBRATION_MS = 1800;

unsigned long lastStatRefresh = 0;
const unsigned long STAT_REFRESH_MS = 500;

// ============== HELPER FUNCTIONS ==============
void hexToBytes(const String& hex, uint8_t* bytes, int len) {
    for(int i = 0; i < len && i*2 < hex.length(); i++) {
        String byteStr = hex.substring(i*2, i*2+2);
        bytes[i] = strtoul(byteStr.c_str(), NULL, 16);
    }
}

String bytesToHex(const uint8_t* bytes, int len) {
    String result = "";
    for(int i = 0; i < len; i++) {
        if(bytes[i] < 16) result += "0";
        result += String(bytes[i], HEX);
    }
    return result;
}

String reverseHex(const String& hex) {
    String result = "";
    for(int i = hex.length() - 2; i >= 0; i -= 2) {
        result += hex.substring(i, i + 2);
    }
    return result;
}

// Single SHA256 using mbedtls (built into ESP32 core)
// Double-SHA is performed by calling this twice from the caller.
void sha256(const uint8_t* data, size_t len, uint8_t* hash) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256, 1 = SHA-224
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
}

void nbitsToTarget(const String& nbits, uint8_t* target) {
    memset(target, 0, 32);
    uint32_t bits = strtoul(nbits.c_str(), NULL, 16);
    uint8_t exponent = (bits >> 24) & 0xFF;
    uint32_t mantissa = bits & 0x00FFFFFF;
    int shift = exponent - 3;
    if(shift >= 0 && shift <= 29) {
        target[32 - shift - 3] = (mantissa >> 16) & 0xFF;
        target[32 - shift - 2] = (mantissa >> 8) & 0xFF;
        target[32 - shift - 1] = mantissa & 0xFF;
    }
}

void difficultyToTarget(double diff, uint8_t* target) {
    memset(target, 0, 32);
    if(diff <= 0) diff = 1;
    double base = 4294901760.0;
    double scaled = base / diff;
    if(diff < 0.00001) {
        target[0] = 0x00;
        target[1] = 0x00;
        memset(target + 2, 0xFF, 30);
    } else if(diff < 1.0) {
        uint64_t val64 = (uint64_t)scaled;
        int shift = 0;
        uint64_t temp = val64;
        while(temp >= 0x100000000ULL && shift < 4) {
            temp >>= 8;
            shift++;
        }
        int startByte = 4 - shift;
        if(startByte < 0) startByte = 0;
        for(int i = 0; i < 6 && (startByte + i) < 32; i++) {
            int byteShift = (5 - i) * 8;
            target[startByte + i] = (val64 >> byteShift) & 0xFF;
        }
        memset(target + startByte + 6, 0x00, 32 - startByte - 6);
    } else {
        uint32_t val = (uint32_t)scaled;
        target[4] = (val >> 24) & 0xFF;
        target[5] = (val >> 16) & 0xFF;
        target[6] = (val >> 8) & 0xFF;
        target[7] = val & 0xFF;
    }
}

String buildMerkleRoot(const String& coinbaseHash, JsonArray& branches) {
    String current = coinbaseHash;
    for(JsonVariant branch : branches) {
        String branchHex = branch.as<String>();
        uint8_t concat[64];
        hexToBytes(current, concat, 32);
        hexToBytes(branchHex, concat + 32, 32);
        uint8_t hash[32];
        uint8_t temp[32];
        sha256(concat, 64, temp);
        sha256(temp, 32, hash);
        current = bytesToHex(hash, 32);
    }
    return current;
}

// ============== STRATUM ==============
bool stratumSend(const String& json) {
    if(!stratumClient.connected()) return false;
    stratumClient.print(json + "\n");
    return true;
}

void stratumSuggestDifficulty() {
    StaticJsonDocument<256> doc;
    doc["id"] = stratumId++;
    doc["method"] = "mining.suggest_difficulty";
    JsonArray params = doc.createNestedArray("params");
    params.add(0.00015);
    String json;
    serializeJson(doc, json);
    stratumSend(json);
}

void stratumSubscribe() {
    StaticJsonDocument<256> doc;
    subscribeId = stratumId++;
    doc["id"] = subscribeId;
    doc["method"] = "mining.subscribe";
    JsonArray params = doc.createNestedArray("params");
    params.add("NerdMinerV2/2.5.0");
    String json;
    serializeJson(doc, json);
    stratumSend(json);
}

void stratumAuthorize() {
    StaticJsonDocument<256> doc;
    authorizeId = stratumId++;
    doc["id"] = authorizeId;
    doc["method"] = "mining.authorize";
    JsonArray params = doc.createNestedArray("params");
    params.add(String(config.btcAddress) + "." + String(config.workerName));
    params.add(config.workerPass);
    String json;
    serializeJson(doc, json);
    stratumSend(json);
}

void stratumSubmit(const String& jobId, uint32_t nonce, const String& extraNonce2) {
    StaticJsonDocument<512> doc;
    uint32_t submitId = stratumId++;
    submitIds.push_back(submitId);
    doc["id"] = submitId;
    doc["method"] = "mining.submit";
    JsonArray params = doc.createNestedArray("params");
    params.add(String(config.btcAddress) + "." + String(config.workerName));
    params.add(jobId);
    params.add(extraNonce2);
    params.add(currentJob.ntime);
    char nonceHex[9];
    snprintf(nonceHex, 9, "%08x", nonce);
    params.add(String(nonceHex));
    String json;
    serializeJson(doc, json);
    stratumSend(json);
}

// Forward declaration for distributeWork
void distributeWork();

void handleStratumNotify(JsonArray& params) {
    if(params.size() < 9) return;
    
    currentJob.jobId = params[0].as<String>();
    currentJob.prevHash = params[1].as<String>();
    currentJob.coinbase1 = params[2].as<String>();
    currentJob.coinbase2 = params[3].as<String>();
    // params[4] is the merkle branch array
    currentJob.version = params[5].as<String>();
    currentJob.nbits = params[6].as<String>();
    currentJob.ntime = params[7].as<String>();
    currentJob.cleanJobs = params[8].as<bool>();
    currentJob.jobIdNum++;
    
    // Generate extraNonce2 in LITTLE-ENDIAN byte order.
    // extraNonce2Counter is 32-bit; if pool's extraNonce2Size > 4, pad upper bytes with zero.
    extraNonce2Counter++;
    char extraNonce2[17] = {0};  // up to 8 bytes = 16 hex chars + null
    for(int i = 0; i < extraNonce2Size; i++) {
        uint8_t byte = (i < 4) ? ((extraNonce2Counter >> (8 * i)) & 0xFF) : 0;
        snprintf(extraNonce2 + (i * 2), 3, "%02x", byte);
    }
    String extraNonce2Str = String(extraNonce2);
    currentJob.extraNonce2 = extraNonce2Str;  // stash for submission later
    
    // Build full coinbase transaction
    String coinbase = currentJob.coinbase1 + extraNonce1 + extraNonce2Str + currentJob.coinbase2;
    uint8_t coinbaseBytes[512];  // 512 to be safe for pools with long coinbases
    int coinbaseLen = coinbase.length() / 2;
    if(coinbaseLen > 512) {
        Serial.printf("!!! Coinbase too large: %d bytes (max 512)\n", coinbaseLen);
        return;
    }
    hexToBytes(coinbase, coinbaseBytes, coinbaseLen);
    
    // Double SHA256 the coinbase
    uint8_t coinbaseHash[32];
    uint8_t temp[32];
    sha256(coinbaseBytes, coinbaseLen, temp);
    sha256(temp, 32, coinbaseHash);
    
    // Walk merkle branches up from raw coinbase hash (NOT reversed)
    JsonArray branches = params[4].as<JsonArray>();
    currentJob.merkleRoot = buildMerkleRoot(bytesToHex(coinbaseHash, 32), branches);
    
    // Network target (from compact nbits) - kept for reference even though slaves use shareTarget
    nbitsToTarget(currentJob.nbits, currentJob.target);
    
    // ===== Build 80-byte block header =====
    // Version: 4 bytes little-endian (full byte reversal)
    hexToBytes(reverseHex(currentJob.version), currentJob.header, 4);
    
    // Previous hash: 32 bytes with 4-byte word-swap pattern
    // ("aabbccdd eeffgghh" becomes "ddccbbaa hhggffee" - each 4-byte word reversed, words stay in order)
    uint8_t prevHashBytes[32];
    hexToBytes(currentJob.prevHash, prevHashBytes, 32);
    for(int i = 0; i < 32; i += 4) {
        currentJob.header[4 + i + 0] = prevHashBytes[i + 3];
        currentJob.header[4 + i + 1] = prevHashBytes[i + 2];
        currentJob.header[4 + i + 2] = prevHashBytes[i + 1];
        currentJob.header[4 + i + 3] = prevHashBytes[i + 0];
    }
    
    // Merkle root: 32 bytes, NO reversal (raw SHA256d output goes straight in)
    uint8_t merkleBytes[32];
    hexToBytes(currentJob.merkleRoot, merkleBytes, 32);
    memcpy(currentJob.header + 36, merkleBytes, 32);
    
    // Time: 4 bytes little-endian (full byte reversal)
    hexToBytes(reverseHex(currentJob.ntime), currentJob.header + 68, 4);
    
    // Bits: 4 bytes little-endian (full byte reversal)
    hexToBytes(reverseHex(currentJob.nbits), currentJob.header + 72, 4);
    
    // Nonce placeholder - slaves fill this
    memset(currentJob.header + 76, 0, 4);
    
    currentJob.valid = true;
    
    Serial.printf("*** NEW JOB: %s (clean=%d) ***\n", 
        currentJob.jobId.c_str(), currentJob.cleanJobs);
    
    distributeWork();
}

void handleStratumSetDifficulty(JsonArray& params) {
    if(params.size() < 1) return;
    poolDifficulty = params[0].as<double>();
    difficultyToTarget(poolDifficulty, shareTarget);
    Serial.printf("Pool difficulty set: %.8f\n", poolDifficulty);
}

void handleStratumResult(uint32_t id, JsonDocument& doc) {
    // Was this a share submission response?
    auto it = std::find(submitIds.begin(), submitIds.end(), id);
    if(it != submitIds.end()) {
        submitIds.erase(it);
        bool result = doc["result"].as<bool>();
        if(result) {
            sharesAccepted++;
            Serial.println("*** Share ACCEPTED ***");
        } else {
            sharesRejected++;
            Serial.print("!!! Share REJECTED: ");
            if(!doc["error"].isNull()) {
                JsonArray errArr = doc["error"].as<JsonArray>();
                if(errArr.size() >= 2) {
                    Serial.printf("Code %d: %s\n", 
                        errArr[0].as<int>(), errArr[1].as<const char*>());
                } else {
                    Serial.println("(unknown)");
                }
            } else {
                Serial.println("(no error details)");
            }
        }
    } else if(id == authorizeId) {
        bool ok = doc["result"].as<bool>();
        Serial.printf("Authorize: %s\n", ok ? "OK" : "FAILED");
        if(!ok && !doc["error"].isNull()) {
            serializeJson(doc["error"], Serial);
            Serial.println();
        }
    }
}

void processStratumLine(const String& line) {
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, line);
    if(error) {
        Serial.printf("!!! JSON parse error: %s\n", error.c_str());
        return;
    }
    
    if(doc.containsKey("result")) {
        uint32_t id = doc["id"] | 0;
        
        if(id == subscribeId) {
            // Subscribe response: result[0]=subscription details, result[1]=extraNonce1, result[2]=extraNonce2Size
            if(doc["result"].is<JsonArray>()) {
                JsonArray result = doc["result"].as<JsonArray>();
                if(result.size() >= 3) {
                    extraNonce1 = result[1].as<String>();
                    extraNonce2Size = result[2].as<int>();
                    if(extraNonce2Size == 0) extraNonce2Size = 4;
                    Serial.printf("Subscribed. extraNonce1=%s, extraNonce2Size=%d\n",
                        extraNonce1.c_str(), extraNonce2Size);
                }
            }
        }
        else if(doc["result"].is<bool>()) {
            handleStratumResult(id, doc);
        }
    }
    else if(doc.containsKey("method")) {
        String method = doc["method"].as<String>();
        JsonArray params = doc["params"].as<JsonArray>();
        
        if(method == "mining.notify") {
            handleStratumNotify(params);
        }
        else if(method == "mining.set_difficulty") {
            handleStratumSetDifficulty(params);
        }
    }
}

void processStratum() {
    while(stratumClient.available()) {
        String line = stratumClient.readStringUntil('\n');
        line.trim();
        if(line.length() > 0) {
            processStratumLine(line);
        }
    }
}

bool connectToPool() {
    Serial.printf("\n=== Connecting to pool %s:%d ===\n", config.poolHost, config.poolPort);
    if(stratumClient.connect(config.poolHost, config.poolPort)) {
        Serial.println("TCP connected");
        stratumSubscribe();
        delay(500);
        stratumAuthorize();
        delay(200);
        stratumSuggestDifficulty();
        Serial.println("Waiting for jobs...");
        return true;
    }
    Serial.println("!!! Pool connection failed");
    return false;
}

// ============================================================
//                       WEB SERVER
//   A small status dashboard (cyberpunk theme to match the LCD)
//   plus a /api JSON endpoint. Served non-blocking via
//   webServer.handleClient() from loop().
// ============================================================
static const char INDEX_HTML[] PROGMEM = R"HTMLDOC(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SHA-256 Cluster</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><rect width='32' height='32' rx='7' fill='%23001018'/><text x='16' y='23' font-size='19' text-anchor='middle' fill='%2300e5ff' font-family='monospace'>%23</text></svg>">
<style>
:root{--panel:#0c1322;--cyan:#00e5ff;--mag:#ff2bd6;--green:#39ff14;--red:#ff3b3b;--grey:#6b7a90;--text:#e8f1ff;--line:#16324a;}
*{box-sizing:border-box;}
body{margin:0;background:radial-gradient(circle at 50% -10%,#0a1530,#05070d 60%);color:var(--text);font-family:'Segoe UI',Roboto,system-ui,monospace;-webkit-font-smoothing:antialiased;}
.wrap{max-width:920px;margin:0 auto;padding:18px;}
.head{display:flex;align-items:center;gap:10px;}
h1{font-size:20px;letter-spacing:3px;margin:0;color:var(--cyan);text-shadow:0 0 8px var(--cyan);}
.live{margin-left:auto;font-size:11px;letter-spacing:2px;color:var(--green);display:flex;align-items:center;gap:7px;}
.live b{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 8px var(--green);animation:blink 1.4s infinite;}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}
.rule{height:2px;border-radius:2px;margin:8px 0 4px;background:linear-gradient(90deg,var(--cyan),var(--mag),transparent);}
.sub{color:var(--grey);font-size:12px;margin:6px 0 16px;word-break:break-all;}
.hero{text-align:center;padding:22px;border:1px solid var(--line);border-radius:12px;background:linear-gradient(180deg,#0c1322,#070c16);animation:heroPulse 3s ease-in-out infinite;}
@keyframes heroPulse{0%,100%{box-shadow:0 0 22px rgba(0,229,255,.07)}50%{box-shadow:0 0 36px rgba(0,229,255,.17)}}
.hero .rate{font-size:48px;font-weight:700;color:var(--green);text-shadow:0 0 16px rgba(57,255,20,.5);}
.hero .unit{font-size:16px;color:var(--grey);}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(118px,1fr));gap:10px;margin:16px 0;}
.stat{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:10px 12px;}
.stat .k{font-size:11px;color:var(--grey);letter-spacing:1px;}
.stat .v{font-size:20px;font-weight:600;margin-top:2px;}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:10px;}
.node{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:11px;transition:transform .15s ease,border-color .15s ease;}
.node:hover{transform:translateY(-2px);border-color:var(--cyan);}
.nrow{display:flex;align-items:center;gap:10px;}
.dot{width:12px;height:12px;border-radius:50%;flex:none;box-shadow:0 0 8px currentColor;}
.node .id{font-size:18px;font-weight:700;line-height:1;}
.node .meta{font-size:10px;color:var(--grey);letter-spacing:.5px;margin-top:3px;}
.node .nr{margin-left:auto;text-align:right;color:var(--cyan);font-weight:600;font-size:13px;}
.tag{font-size:9px;letter-spacing:1px;padding:1px 5px;border-radius:5px;border:1px solid var(--line);color:var(--grey);}
.bar{height:4px;border-radius:3px;background:#0a1830;margin-top:9px;overflow:hidden;}
.bar i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--cyan),var(--mag));box-shadow:0 0 8px var(--cyan);transition:width .6s ease;}
.foot{margin-top:16px;color:var(--grey);font-size:11px;text-align:center;word-break:break-all;}
</style></head><body><div class="wrap">
<div class="head"><h1>SHA-256 MINING CLUSTER</h1><div class="live"><b></b>LIVE</div></div>
<div class="rule"></div>
<div class="sub" id="sub">connecting...</div>
<div class="hero"><div class="rate"><span id="rate">--</span> <span class="unit">kH/s</span></div></div>
<div class="stats">
<div class="stat"><div class="k">NODES ONLINE</div><div class="v" id="online">-/10</div></div>
<div class="stat"><div class="k">SHARES FOUND</div><div class="v" id="found">-</div></div>
<div class="stat"><div class="k">ACCEPTED</div><div class="v" style="color:var(--green)" id="acc">-</div></div>
<div class="stat"><div class="k">REJECTED</div><div class="v" id="rej">-</div></div>
<div class="stat"><div class="k">DIFFICULTY</div><div class="v" id="diff">-</div></div>
<div class="stat"><div class="k">UPTIME</div><div class="v" id="up">-</div></div>
</div>
<div class="grid" id="grid"></div>
<div class="foot" id="foot"></div>
</div><script>
var ST={0:'READY',1:'MINING',2:'SHARE',3:'DONE'};
function fmtRate(h){return (h/1000).toFixed(2);}
function fmtUp(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),x=s%60,o='';if(d)o+=d+'d ';if(d||h)o+=h+'h ';return o+m+'m '+x+'s';}
function esc(t){return String(t).replace(/[&<>]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;'}[c];});}
async function tick(){
 var ctrl=new AbortController(),to=setTimeout(function(){ctrl.abort();},4000);
 try{
  var j=await (await fetch('/api',{signal:ctrl.signal})).json();clearTimeout(to);
  document.getElementById('rate').textContent=fmtRate(j.hashrate);
  document.getElementById('online').textContent=j.online+'/10';
  document.getElementById('found').textContent=j.sharesFound;
  document.getElementById('acc').textContent=j.sharesAccepted;
  document.getElementById('rej').textContent=j.sharesRejected;
  document.getElementById('diff').textContent=(+j.difficulty).toFixed(5);
  document.getElementById('up').textContent=fmtUp(j.uptime);
  document.getElementById('sub').textContent=j.pool+'   -   '+j.ip;
  document.getElementById('foot').textContent=j.worker;
  var max=1;j.slaves.forEach(function(s){if(s.online&&s.hashrate>max)max=s.hashrate;});
  var g=document.getElementById('grid');g.innerHTML='';
  j.slaves.forEach(function(s){
   var hex=s.addr.toString(16).toUpperCase().padStart(2,'0'),on=s.online;
   var tag=on?(ST[s.status]||'-'):'OFFLINE',col=on?'#39ff14':'#ff3b3b';
   var pct=on?Math.round(s.hashrate/max*100):0;
   var d=document.createElement('div');d.className='node';
   d.innerHTML='<div class="nrow"><div class="dot" style="color:'+col+'"></div>'+
     '<div><div class="id">'+hex+'</div><div class="meta">BUS '+s.bus+' &middot; '+(s.bus==0?'S3':'C3')+' &middot; <span class="tag">'+esc(tag)+'</span></div></div>'+
     '<div class="nr">'+(on?fmtRate(s.hashrate)+'<br><span style="color:var(--grey);font-weight:400">kH/s</span>':'<span style="color:var(--red)">--</span>')+'</div></div>'+
     '<div class="bar"><i style="width:'+pct+'%"></i></div>';
   g.appendChild(d);
  });
 }catch(e){clearTimeout(to);document.getElementById('sub').textContent='link lost... retrying';}
}
tick();setInterval(tick,2000);
</script></body></html>)HTMLDOC";

void handleRoot() {
    webServer.send_P(200, "text/html", INDEX_HTML);
}

void handleApi() {
    StaticJsonDocument<3072> doc;
    doc["hashrate"]        = totalHashrate;
    int online = 0;
    for(int i = 0; i < 10; i++) if(slaves[i].online) online++;
    doc["online"]         = online;
    doc["sharesFound"]    = sharesFound;
    doc["sharesAccepted"] = sharesAccepted;
    doc["sharesRejected"] = sharesRejected;
    doc["pool"]           = String(config.poolHost) + ":" + String(config.poolPort);
    doc["worker"]         = String(config.btcAddress) + "." + String(config.workerName);
    doc["difficulty"]     = poolDifficulty;
    doc["uptime"]         = millis() / 1000;
    doc["ip"]             = WiFi.localIP().toString();

    JsonArray arr = doc.createNestedArray("slaves");
    for(int i = 0; i < 10; i++) {
        JsonObject o = arr.createNestedObject();
        o["addr"]     = slaves[i].address;
        o["bus"]      = slaves[i].bus;
        o["online"]   = slaves[i].online;
        o["hashrate"] = slaves[i].smoothedHashrate;
        o["status"]   = slaves[i].status;
    }

    String out;
    serializeJson(doc, out);
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", out);
}

void setupWebServer() {
    webServer.on("/", handleRoot);
    webServer.on("/api", handleApi);
    // The page supplies its own inline icon; answer the browser's default
    // /favicon.ico probe with 204 so it doesn't fall through to a 404.
    webServer.on("/favicon.ico", []() { webServer.send(204, "image/x-icon", ""); });
    webServer.onNotFound([]() { webServer.send(404, "text/plain", "Not found"); });
    webServer.begin();
    Serial.println("Web dashboard: http://" + WiFi.localIP().toString() + "/");
}

// ============== I2C CLUSTER MANAGEMENT ==============
void sendJobToSlave(int slaveIdx); // forward decl

// The ESP32 core 3.x i2c_master driver can wedge after a failed transaction
// (a missing/slow slave, an address collision, a glitch) and then every
// following transaction on that bus fails until the driver is torn down and
// rebuilt. That is exactly what makes "only some slaves show up, randomly":
// the first slave that doesn't answer cleanly locks the bus for the rest of
// the cycle. This helper re-inits a bus so a single bad slave can't cascade.
void recoverBus(uint8_t busNum) {
    if(busNum == 0) {
        Wire.end();
        delay(2);
        Wire.begin(I2C_0_SDA, I2C_0_SCL, 100000);
    } else {
        Wire1.end();
        delay(2);
        Wire1.begin(I2C_1_SDA, I2C_1_SCL, 100000);
    }
    delay(2);
}

void distributeWork() {
    if(!currentJob.valid) return;
    uint32_t rangeSize = 0xFFFFFFFF / 10; // Hardcoded for 10 slaves
    for(int i = 0; i < 10; i++) {
        slaves[i].nonceStart = i * rangeSize;
        slaves[i].nonceEnd = (i == 9) ? 0xFFFFFFFF : ((i + 1) * rangeSize - 1);
        slaves[i].lastJobId = currentJob.jobIdNum;
        slaves[i].lastSubmittedNonce = 0;
        slaves[i].lastSubmittedJobId = 0;
        sendJobToSlave(i);
    }
}

void sendJobToSlave(int slaveIdx) {
    uint8_t packet[125];
    packet[0] = CMD_NEW_JOB;
    memcpy(packet + 1, currentJob.header, 80);      
    memcpy(packet + 81, shareTarget, 32);
    memcpy(packet + 113, &slaves[slaveIdx].nonceStart, 4);
    memcpy(packet + 117, &slaves[slaveIdx].nonceEnd, 4);
    memcpy(packet + 121, &currentJob.jobIdNum, 4);
    
    uint8_t busNum = slaves[slaveIdx].bus;
    
    // Try up to 3 times; recover the bus between failures so a wedged driver
    // from this slave doesn't take down every slave after it in distributeWork.
    uint8_t error = 0xFF;
    for(int attempt = 0; attempt < 3; attempt++) {
        TwoWire& targetWire = (busNum == 0) ? Wire : Wire1;
        targetWire.beginTransmission(slaves[slaveIdx].address);
        targetWire.write(packet, 125); 
        error = targetWire.endTransmission();
        if(error == 0) break;
        recoverBus(busNum);
    }
    
    slaves[slaveIdx].online = (error == 0);
    if(error != 0) {
        Serial.printf("!!! Slave 0x%02X (bus %d) offline (I2C err %d)\n", 
            slaves[slaveIdx].address, slaves[slaveIdx].bus, error);
    }
}

// Apply one hashrate sample (H/s) from a slave. Slaves measure their own rate
// as (totalHashes*1000)/elapsed, so values legitimately differ a lot across the
// fleet (dual-core S3s ~2x the single-core C3s). We therefore DON'T window the
// value to an "expected" band -- that silently hides slow nodes and caps fast
// ones. We only reject readings large enough to be obvious I2C corruption, then
// smooth with an EMA (seeded instantly so a node doesn't crawl up from zero).
void updateSlaveHashrate(int idx, uint32_t raw) {
    const uint32_t MAX_SANE_HPS = 2000000;   // 2 MH/s: a single ESP32 can't reach this -> corrupt read
    if(raw > MAX_SANE_HPS) return;           // drop garbage, keep last good value
    slaves[idx].hashrate = raw;              // instantaneous
    if(raw == 0) {
        slaves[idx].smoothedHashrate = 0;
    } else if(slaves[idx].smoothedHashrate == 0) {
        slaves[idx].smoothedHashrate = raw;  // seed on first valid sample
    } else {
        slaves[idx].smoothedHashrate = (raw * 7 + slaves[idx].smoothedHashrate * 3) / 10;
    }
}

void pollBus(uint8_t busNum, TwoWire& activeWire, int attnPin) {
    digitalWrite(attnPin, HIGH);
    delayMicroseconds(1500); 
    
    for(int i = 0; i < 10; i++) {
        if(slaves[i].bus != busNum) continue;
        
        // Small inter-slave settle delay (re-added for 5-slave-per-bus reliability)
        delayMicroseconds(200);

        int bytesRead = activeWire.requestFrom((int)slaves[i].address, 45);
        if(bytesRead >= 1) {
            uint8_t status = activeWire.read();
            slaves[i].status = status;
            slaves[i].online = true;
            
            if(status == RESP_FOUND && bytesRead >= 45) {
                uint32_t nonce;
                uint32_t hashrate;
                uint32_t jobId;
                uint8_t hash[32];
                
                activeWire.readBytes((uint8_t*)&nonce, 4);
                activeWire.readBytes((uint8_t*)&hashrate, 4);
                activeWire.readBytes((uint8_t*)&jobId, 4);
                activeWire.readBytes(hash, 32);

                // The FOUND frame also carries the slave's current hashrate.
                updateSlaveHashrate(i, hashrate);

                if(jobId == currentJob.jobIdNum && 
                   !(nonce == slaves[i].lastSubmittedNonce && jobId == slaves[i].lastSubmittedJobId)) {
                    slaves[i].lastSubmittedNonce = nonce;
                    slaves[i].lastSubmittedJobId = jobId;
                    stratumSubmit(currentJob.jobId, nonce, currentJob.extraNonce2);
                    sharesFound++;
                    memcpy(lastFoundHash, hash, 32);   // stash for the LCD celebration
                    haveFoundHash = true;
                    Serial.printf("*** SHARE FOUND BY SLAVE 0x%02X (bus %d) nonce=0x%08X ***\n", 
                        slaves[i].address, slaves[i].bus, nonce);
                } else if(jobId != currentJob.jobIdNum) {
                    Serial.printf("Stale share from 0x%02X (job %u vs current %u)\n",
                        slaves[i].address, jobId, currentJob.jobIdNum);
                }
            }
            else if(status == RESP_BUSY || status == RESP_READY) {
                if(bytesRead >= 5) {
                    uint8_t b0 = activeWire.read();
                    uint8_t b1 = activeWire.read();
                    uint8_t b2 = activeWire.read();
                    uint8_t b3 = activeWire.read();
                    uint32_t rawHashrate = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
                    updateSlaveHashrate(i, rawHashrate);
                }
            }
        } else {
            slaves[i].online = false;
            // Failed read may have wedged the bus driver. Recover before the
            // next slave so one missing board can't black out the whole bus.
            recoverBus(busNum);
        }
        while(activeWire.available()) activeWire.read(); 
        slaves[i].lastPoll = millis();
    }
    
    digitalWrite(attnPin, LOW);
}

// ============================================================
//                      LCD DISPLAY MODULE
//   Cyberpunk SHA-256 dashboard: a churning live digest, key
//   stats, an auto-rotating 10-node screen, and a share-found
//   celebration. All drawing is incremental / time-sliced so
//   the mining loop (stratum + dual-I2C) is never starved.
// ============================================================

// ---- low-level text helpers (GFX default font = 6*size px wide) ----
void tftText(int x, int y, uint8_t size, uint16_t color, const char* s) {
    tft.setTextSize(size);
    tft.setTextColor(color, UI_BG);
    tft.setCursor(x, y);
    tft.print(s);
}

void tftTextCentered(int y, uint8_t size, uint16_t color, const char* s) {
    int w = (int)strlen(s) * 6 * size;
    int x = (240 - w) / 2;
    if (x < 0) x = 0;
    tftText(x, y, size, color, s);
}

void drawCornerBrackets(uint16_t c) {
    const int L = 12;
    tft.drawFastHLine(0, 0, L, c);        tft.drawFastVLine(0, 0, L, c);
    tft.drawFastHLine(240 - L, 0, L, c);  tft.drawFastVLine(239, 0, L, c);
    tft.drawFastHLine(0, 239, L, c);      tft.drawFastVLine(0, 240 - L, L, c);
    tft.drawFastHLine(240 - L, 239, L, c);tft.drawFastVLine(239, 240 - L, L, c);
}

char hexChar(int v) { return "0123456789abcdef"[v & 0xF]; }

void initHashDigest() {
    for (int i = 0; i < 64; i++) hashDigest[i] = hexChar(random(16));
    hashDigest[64] = '\0';
}

// ---- boot / status screens (shown while WiFi + pool connect) ----
void showBootScreen() {
    tft.fillScreen(UI_BG);
    drawCornerBrackets(UI_CYAN);
    tftTextCentered(64, 3, UI_CYAN, "SHA-256");
    tftTextCentered(96, 2, UI_MAGENTA, "MINING RIG");
    tftTextCentered(124, 1, UI_GREY, "10 x ESP32   DUAL I2C");
}

void showBootStatus(const char* msg, uint16_t color) {
    tft.fillRect(0, 158, 240, 18, UI_BG);
    tftTextCentered(162, 1, color, msg);
}

// ---- main dashboard: digest geometry ----
// 64 hex chars laid out as 2 rows of 32, size 1 (6px advance).
#define DGT_X 24
#define DGT_Y 100
#define DGT_ROW_H 11
static inline int dgtX(int idx) { return DGT_X + (idx % 32) * 6; }
static inline int dgtY(int idx) { return DGT_Y + (idx / 32) * DGT_ROW_H; }

void drawDigestFull() {
    tft.setTextSize(1);
    for (int idx = 0; idx < 64; idx++) {
        tft.setTextColor(UI_HASH, UI_BG);
        tft.setCursor(dgtX(idx), dgtY(idx));
        tft.print(hashDigest[idx]);
    }
}

#define CHURN_N 12
int  prevChurn[CHURN_N];
bool prevChurnValid = false;

void churnHashDigest() {
    tft.setTextSize(1);
    // fade the previously-bright digits back to dim
    if (prevChurnValid) {
        for (int k = 0; k < CHURN_N; k++) {
            int idx = prevChurn[k];
            tft.setTextColor(UI_HASH, UI_BG);
            tft.setCursor(dgtX(idx), dgtY(idx));
            tft.print(hashDigest[idx]);
        }
    }
    // scramble a fresh set and draw them bright
    for (int k = 0; k < CHURN_N; k++) {
        int idx = random(64);
        hashDigest[idx] = hexChar(random(16));
        prevChurn[k] = idx;
        tft.setTextColor(UI_HASHHI, UI_BG);
        tft.setCursor(dgtX(idx), dgtY(idx));
        tft.print(hashDigest[idx]);
    }
    prevChurnValid = true;
}

void drawMainStatic() {
    tft.fillScreen(UI_BG);
    drawCornerBrackets(UI_CYAN);
    // header
    tftText(8, 5, 2, UI_CYAN, "SHA-256");
    tftText(150, 5, 1, UI_MAGENTA, "CLUSTER");
    tftText(150, 15, 1, UI_GREY, "MINER");
    tft.drawFastHLine(0, 27, 240, UI_CYAN);
    // hashrate hero
    tftText(8, 33, 1, UI_GREY, "HASHRATE");
    tftText(186, 60, 1, UI_GREY, "kH/s");
    tft.drawFastHLine(0, 84, 240, UI_GREY);
    // live digest
    tftText(8, 89, 1, UI_GREY, "LIVE DIGEST");
    tft.drawFastHLine(0, 124, 240, UI_GREY);
    // stat grid labels
    tftText(8, 132, 1, UI_GREY, "NODES");
    tftText(128, 132, 1, UI_GREY, "ACCEPT");
    tftText(8, 176, 1, UI_GREY, "FOUND");
    tftText(128, 176, 1, UI_GREY, "REJECT");
    drawDigestFull();
}

void drawMainStats() {
    char buf[16], pad[16];
    // total hashrate (kH/s) -- the hero number
    if (totalHashrate != shownHashrate) {
        shownHashrate = totalHashrate;
        snprintf(buf, sizeof buf, "%6.2f", totalHashrate / 1000.0);
        tftText(8, 46, 3, totalHashrate > 0 ? UI_GREEN : UI_GREY, buf);
    }
    // nodes online x/10
    int online = 0;
    for (int i = 0; i < 10; i++) if (slaves[i].online) online++;
    if (online != shownOnline) {
        shownOnline = online;
        snprintf(buf, sizeof buf, "%d/10", online);
        snprintf(pad, sizeof pad, "%-5s", buf);
        uint16_t c = online == 10 ? UI_GREEN : (online == 0 ? UI_RED : UI_YELLOW);
        tftText(8, 146, 2, c, pad);
    }
    if (sharesAccepted != shownAccepted) {
        shownAccepted = sharesAccepted;
        snprintf(buf, sizeof buf, "%lu", (unsigned long)sharesAccepted);
        tftText(128, 146, 2, UI_GREEN, buf);
    }
    if (sharesFound != shownFound) {
        shownFound = sharesFound;
        snprintf(buf, sizeof buf, "%lu", (unsigned long)sharesFound);
        tftText(8, 190, 2, UI_CYAN, buf);
    }
    if (sharesRejected != shownRejected) {
        shownRejected = sharesRejected;
        snprintf(buf, sizeof buf, "%lu", (unsigned long)sharesRejected);
        tftText(128, 190, 2, sharesRejected > 0 ? UI_RED : UI_GREY, buf);
    }
}

// ---- node grid view (occasional) ----
void drawNodesStatic() {
    tft.fillScreen(UI_BG);
    drawCornerBrackets(UI_MAGENTA);
    tftText(8, 5, 2, UI_MAGENTA, "NODES");
    tftText(150, 5, 1, UI_CYAN, "10 CORES");
    tftText(150, 15, 1, UI_GREY, "DUAL BUS");
    tft.drawFastHLine(0, 27, 240, UI_MAGENTA);
    tftText(20, 32, 1, UI_GREY, "S3 / BUS 0");
    tftText(140, 32, 1, UI_GREY, "C3 / BUS 1");
    for (int i = 0; i < 10; i++) { shownNodeStatus[i] = 0xFF; shownNodeRate[i] = 0xFFFFFFFF; }
}

void drawNodeCell(int idx, int x, int y) {
    uint8_t  on   = slaves[idx].online ? 1 : 0;
    uint32_t rate = slaves[idx].smoothedHashrate;
    if (on == shownNodeStatus[idx] && rate == shownNodeRate[idx]) return;
    shownNodeStatus[idx] = on;
    shownNodeRate[idx]   = rate;

    uint16_t col = on ? UI_GREEN : UI_RED;
    tft.drawRect(x, y, 108, 32, col);
    tft.fillCircle(x + 11, y + 11, 4, col);

    char a[6];
    snprintf(a, sizeof a, "%02X", slaves[idx].address);
    tftText(x + 24, y + 5, 2, UI_WHITE, a);

    char r[16];
    if (on) snprintf(r, sizeof r, "%5.1f kH/s", rate / 1000.0);
    else    snprintf(r, sizeof r, "OFFLINE    ");
    tftText(x + 8, y + 21, 1, on ? UI_CYAN : UI_GREY, r);
}

void drawNodesStats() {
    for (int i = 0; i < 5; i++)  drawNodeCell(i, 8,   46 + i * 36);
    for (int i = 5; i < 10; i++) drawNodeCell(i, 124, 46 + (i - 5) * 36);
}

// ---- share-found celebration overlay (non-blocking) ----
unsigned long lastCelebFrame = 0;

void startCelebration() {
    celebrating = true;
    celebrationStart = millis();
    lastCelebFrame = 0;
    prevChurnValid = false;   // digest will be redrawn fresh when we return

    tft.fillScreen(UI_BG);
    drawCornerBrackets(UI_YELLOW);
    tftTextCentered(30, 3, UI_YELLOW, "SHARE");
    tftTextCentered(62, 3, UI_GREEN,  "FOUND!");

    // show the actual winning hash, dimmed, as 2 rows of 32 hex
    tftTextCentered(100, 1, UI_GREY, "WINNING HASH");
    if (haveFoundHash) {
        String h = bytesToHex(lastFoundHash, 32);
        h.toCharArray(hashDigest, 65);
    }
    tft.setTextSize(1);
    for (int idx = 0; idx < 64; idx++) {
        tft.setTextColor(UI_CYAN, UI_BG);
        tft.setCursor(DGT_X + (idx % 32) * 6, 116 + (idx / 32) * 11);
        tft.print(hashDigest[idx]);
    }
}

void celebrationFrame() {
    unsigned long now = millis();
    if (now - lastCelebFrame < 30) return;   // throttle the ring animation
    lastCelebFrame = now;
    unsigned long e = now - celebrationStart;
    int r = (e / 14) % 56;                    // expanding ring radius, repeating
    uint16_t cols[3] = { UI_YELLOW, UI_GREEN, UI_CYAN };
    tft.drawCircle(120, 192, r, cols[(e / 700) % 3]);
}

// ---- the scheduler: called every loop(), time-sliced internally ----
void enterView(DisplayView v) {
    currentView = v;
    viewJustEntered = true;
    viewEnteredAt = millis();
}

void updateDisplay() {
    unsigned long now = millis();

    // 1) New share? Kick off the celebration and pre-empt everything else.
    if (sharesFound != lastCelebratedShares) {
        lastCelebratedShares = sharesFound;
        startCelebration();
        return;
    }

    // 2) Celebration in progress.
    if (celebrating) {
        if (now - celebrationStart < CELEBRATION_MS) {
            celebrationFrame();
            return;
        }
        celebrating = false;
        viewJustEntered = true;       // restore whatever view we were on
        viewEnteredAt = now;
        initHashDigest();             // give the digest fresh churn material
    }

    // 3) Auto-rotate between the dashboard and the node grid.
    if (!viewJustEntered) {
        unsigned long dwell = (currentView == VIEW_MAIN) ? MAIN_VIEW_MS : NODES_VIEW_MS;
        if (now - viewEnteredAt > dwell) {
            enterView(currentView == VIEW_MAIN ? VIEW_NODES : VIEW_MAIN);
        }
    }

    // 4) Static redraw on view entry.
    if (viewJustEntered) {
        viewJustEntered = false;
        shownHashrate = 0xFFFFFFFF; shownFound = 0xFFFFFFFF;
        shownAccepted = 0xFFFFFFFF; shownRejected = 0xFFFFFFFF; shownOnline = -1;
        if (currentView == VIEW_MAIN) drawMainStatic();
        else                          drawNodesStatic();
        lastStatRefresh = 0;
    }

    // 5) Dynamic updates (throttled).
    if (currentView == VIEW_MAIN) {
        if (now - lastChurn >= CHURN_MS) { lastChurn = now; churnHashDigest(); }
        if (now - lastStatRefresh >= STAT_REFRESH_MS) { lastStatRefresh = now; drawMainStats(); }
    } else {
        if (now - lastStatRefresh >= STAT_REFRESH_MS) { lastStatRefresh = now; drawNodesStats(); }
    }
}

// ============== SETUP & LOOP ==============
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n================================");
    Serial.println("SHA256 Mining Cluster Master");
    Serial.println("ESP32 dev kit, dual I2C, 10 slaves");
    Serial.println("================================\n");

    // ----- LCD up first so we have something on screen during boot -----
    randomSeed(esp_random());
    tft.init(240, 240);
    tft.setRotation(0);          // match scanner v4; try 1/2/3 if orientation is off
    initHashDigest();
    showBootScreen();
    showBootStatus("BOOTING...", UI_GREY);

    pinMode(ATTN_0_PIN, OUTPUT);
    digitalWrite(ATTN_0_PIN, LOW);
    pinMode(ATTN_1_PIN, OUTPUT);
    digitalWrite(ATTN_1_PIN, LOW);

    Wire.begin(I2C_0_SDA, I2C_0_SCL, 100000); 
    Wire1.begin(I2C_1_SDA, I2C_1_SCL, 100000);
    delay(1500);  // let all slaves finish booting before any I2C traffic

    difficultyToTarget(poolDifficulty, shareTarget);

    WiFi.begin(config.ssid, config.password);
    Serial.print("Connecting to WiFi");
    showBootStatus("WIFI: connecting...", UI_YELLOW);
    while(WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
    showBootStatus(("WIFI OK  " + WiFi.localIP().toString()).c_str(), UI_GREEN);
    delay(400);

    showBootStatus("POOL: connecting...", UI_YELLOW);
    connectToPool();
    showBootStatus("POOL CONNECTED - MINING", UI_GREEN);
    delay(500);

    setupWebServer();

    currentJob.valid = false;

    // hand off to the live dashboard
    lastCelebratedShares = sharesFound;
    enterView(VIEW_MAIN);
}

void loop() {
    webServer.handleClient();

    if(!stratumClient.connected()) {
        Serial.println("Pool disconnected, reconnecting...");
        showBootScreen();
        showBootStatus("POOL LOST - RECONNECTING", UI_RED);
        connectToPool();
        viewJustEntered = true;   // force the dashboard to repaint cleanly
        delay(5000);
        return;
    }
    
    processStratum();
    
    static unsigned long lastPoll = 0;
    if(millis() - lastPoll > 500) {
        lastPoll = millis();
        pollBus(0, Wire, ATTN_0_PIN);
        pollBus(1, Wire1, ATTN_1_PIN);
        
        totalHashrate = 0;
        for(int i = 0; i < 10; i++) {
            if(slaves[i].online) totalHashrate += slaves[i].smoothedHashrate;
        }
    }

    updateDisplay();
    
    static unsigned long lastStatus = 0;
    if(millis() - lastStatus > 10000) {
        lastStatus = millis();
        int online = 0;
        for(int i = 0; i < 10; i++) if(slaves[i].online) online++;
        Serial.printf("\n[STATUS] Hashrate: %.2f kH/s | Slaves: %d/10 | Shares: %u (A:%u R:%u)\n", 
            totalHashrate / 1000.0, online, sharesFound, sharesAccepted, sharesRejected);
    }
    delay(10);
}
