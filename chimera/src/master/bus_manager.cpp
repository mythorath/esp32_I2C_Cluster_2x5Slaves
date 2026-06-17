#include "bus_manager.h"

namespace chimera {

void BusManager::begin() {
    pinMode(ATTN0, OUTPUT); digitalWrite(ATTN0, LOW);
    pinMode(ATTN1, OUTPUT); digitalWrite(ATTN1, LOW);
    Wire.setBufferSize(WIRE_BUFFER);
    Wire1.setBufferSize(WIRE_BUFFER);
    Wire.begin(PIN_I2C0_SDA, PIN_I2C0_SCL, I2C_HZ);
    Wire1.begin(PIN_I2C1_SDA, PIN_I2C1_SCL, I2C_HZ);
    delay(50);
}

void BusManager::recoverBus(uint8_t bus) {
    // The ESP32 core i2c_master driver can wedge after a failed transaction;
    // tearing down and re-initing clears it so one bad node can't cascade.
    if (bus == 0) { Wire.end(); delay(2); Wire.begin(PIN_I2C0_SDA, PIN_I2C0_SCL, I2C_HZ); }
    else          { Wire1.end(); delay(2); Wire1.begin(PIN_I2C1_SDA, PIN_I2C1_SCL, I2C_HZ); }
    delay(2);
}

bool BusManager::probe(int strip) {
    TwoWire& w = wireFor(strip);
    for (int i = 0; i < MAX_RETRY; i++) {
        w.beginTransmission(stripAddr(strip));
        if (w.endTransmission() == 0) { online_[strip] = true; return true; }
        recoverBus(stripBus(strip));
    }
    online_[strip] = false;
    return false;
}

void BusManager::scanAll() {
    for (int i = 0; i < N_STRIPS; i++) probe(i);
}

int BusManager::onlineCount() const {
    int n = 0;
    for (int i = 0; i < N_STRIPS; i++) if (online_[i]) n++;
    return n;
}

bool BusManager::writeCmd(int strip, const uint8_t* data, int len, bool demote) {
    TwoWire& w = wireFor(strip);
    for (int attempt = 0; attempt < MAX_RETRY; attempt++) {
        w.beginTransmission(stripAddr(strip));
        w.write(data, len);
        if (w.endTransmission() == 0) return true;
        recoverBus(stripBus(strip));
    }
    // Only demote when a control transaction (tick/probe) fails. Bulk transfers
    // (halo/tile) pass demote=false: a flaky halo must NOT drop the node - it
    // stays online and self-wraps that generation. Presence is owned by
    // probe()/reconcile() (address ACK), which the C3s handle reliably.
    if (demote) online_[strip] = false;
    else { bulkFails++; lastFailStrip = strip; }
    return false;
}

void BusManager::tick(int strip, const Genome* genome) {
    uint8_t buf[1 + GENOME_BYTES];
    buf[0] = CMD_TICK;
    int len = 1;
    if (genome) { serializeGenome(*genome, buf + 1); len += GENOME_BYTES; }
    writeCmd(strip, buf, len);
}

void BusManager::tickAll(const Genome* genomes) {
    for (int i = 0; i < N_STRIPS; i++)
        if (online_[i]) tick(i, genomes ? &genomes[i] : nullptr);
}

void BusManager::seed(int strip, uint8_t pattern, uint32_t seedVal) {
    uint8_t buf[6] = {CMD_SEED, pattern,
                      (uint8_t)(seedVal), (uint8_t)(seedVal >> 8),
                      (uint8_t)(seedVal >> 16), (uint8_t)(seedVal >> 24)};
    writeCmd(strip, buf, 6);
}

void BusManager::setGenome(int strip, const Genome& g) {
    uint8_t buf[1 + GENOME_BYTES];
    buf[0] = CMD_SET_GENOME;
    serializeGenome(g, buf + 1);
    writeCmd(strip, buf, 1 + GENOME_BYTES);
}

uint8_t BusManager::readStatus(int strip) {
    TwoWire& w = wireFor(strip);
    uint8_t cmd = CMD_GET_STATUS;
    w.beginTransmission(stripAddr(strip));
    w.write(&cmd, 1);
    if (w.endTransmission() != 0) { recoverBus(stripBus(strip)); return ST_ERROR; }
    int n = w.requestFrom((int)stripAddr(strip), 1);
    if (n < 1) { recoverBus(stripBus(strip)); return ST_ERROR; }
    return w.read();
}

bool BusManager::barrier(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        bool allReady = true;
        for (int i = 0; i < N_STRIPS; i++) {
            if (!online_[i]) continue;
            if (readStatus(i) != ST_READY) { allReady = false; }
        }
        if (allReady) return true;
        delayMicroseconds(300);
    }
    // Timeout: do NOT demote here. A node that still ACKs its address is present;
    // a transient "not READY" shouldn't drop it (that mass-demotes a whole slow
    // bank at once). reconcile() owns presence via address probe.
    return false;
}

bool BusManager::readStats(int strip, StripStats& out) {
    TwoWire& w = wireFor(strip);
    uint8_t cur[4] = {CMD_SET_CURSOR, CMD_GET_STATS, 0, 0};
    if (!writeCmd(strip, cur, 4)) return false;
    int n = w.requestFrom((int)stripAddr(strip), STATS_BYTES);
    if (n < STATS_BYTES) { recoverBus(stripBus(strip)); return false; }
    uint8_t* p = (uint8_t*)&out;
    for (int i = 0; i < STATS_BYTES; i++) p[i] = w.read();
    return true;
}

bool BusManager::readFrame(int strip, uint8_t op, uint8_t sub, uint16_t chunk,
                           uint8_t* frame, int cap) {
    TwoWire& w = wireFor(strip);
    uint8_t cur[5] = {CMD_SET_CURSOR, op, sub, (uint8_t)(chunk & 0xFF), (uint8_t)(chunk >> 8)};
    if (!writeCmd(strip, cur, 5, false)) return false;   // bulk: never demote
    int n = w.requestFrom((int)stripAddr(strip), cap);
    if (n < FRAME_OVERHEAD) { recoverBus(stripBus(strip)); bulkFails++; lastFailStrip = strip; return false; }
    for (int i = 0; i < n && i < cap; i++) frame[i] = w.read();
    return true;
}

bool BusManager::readBuffer(int strip, uint8_t op, uint8_t sub, uint8_t* dst, int total) {
    int chunks = chunkCount(total);
    uint8_t frame[CHUNK_FRAME_MAX];
    attn(strip, true);
    bool ok = true;
    for (int seq = 0; seq < chunks; seq++) {
        bool got = false;
        for (int r = 0; r < MAX_RETRY && !got; r++) {
            if (!readFrame(strip, op, sub, seq, frame, CHUNK_FRAME_MAX)) continue;
            if (frame[FRAME_SEQ] != seq) continue;
            if (parseFrame(frame, CHUNK_FRAME_MAX, dst, total) >= 0) got = true;
        }
        if (!got) { ok = false; break; }
    }
    attn(strip, false);
    return ok;
}

bool BusManager::writeBuffer(int strip, uint8_t op, uint8_t sub, const uint8_t* src, int total) {
    int chunks = chunkCount(total);
    uint8_t out[2 + CHUNK_FRAME_MAX];
    bool ok = true;
    for (int seq = 0; seq < chunks; seq++) {
        out[0] = op;
        out[1] = sub;
        int flen = buildFrame(src, total, seq, out + 2);
        if (!writeCmd(strip, out, 2 + flen, false)) { ok = false; break; }   // bulk: never demote
    }
    return ok;
}

}  // namespace chimera
