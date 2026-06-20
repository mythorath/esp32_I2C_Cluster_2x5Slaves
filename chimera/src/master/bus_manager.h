// bus_manager.h - dual-I2C transport + polled-barrier for the master.
//
// The master is I2C master on BOTH buses (Wire=bus0=S3, Wire1=bus1=C3). Nodes
// cannot clock-stretch, so synchronization is a SOFTWARE polled barrier: after
// broadcasting CMD_TICK, poll every online node's status byte until all report
// ST_READY (or timeout). The per-bank attention line is asserted during multi-
// chunk read bursts so node compute can't race the I2C ISR.
//
// All large transfers (halos, tiles) are chunked [seq][len][payload][crc8] with
// per-frame CRC + retransmit, since Wire buffers are small and the bus is noisy.
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "protocol.h"
#include "genome.h"
#include "topology.h"

namespace chimera {

class BusManager {
public:
    void begin();

    // Presence / health.
    bool probe(int strip);
    void scanAll();                       // refresh online[] for all strips
    bool online(int strip) const { return online_[strip]; }
    int  onlineCount() const;

    // Commands.
    void tick(int strip, const Genome* genome = nullptr);
    void tickAll(const Genome* genomes = nullptr);   // broadcast to all online
    void seed(int strip, uint8_t pattern, uint32_t seedVal = 0);
    void setGenome(int strip, const Genome& g);

    // Polled barrier: wait until all online nodes report ST_READY.
    // Returns true if all ready before timeout.
    bool barrier(uint32_t timeoutMs = 2000);
    uint8_t readStatus(int strip);

    // Bulk reads/writes (chunked, CRC-checked, retransmitting).
    bool readStats(int strip, StripStats& out);
    bool readBuffer(int strip, uint8_t op, uint8_t sub, uint8_t* dst, int total);
    bool writeBuffer(int strip, uint8_t op, uint8_t sub, const uint8_t* src, int total);

    // Raw recovery for a wedged bus driver.
    void recoverBus(uint8_t bus);

    // Diagnostics: bulk (halo/tile) transfer failures that did NOT demote a node.
    uint32_t bulkFails = 0;
    int lastFailStrip = -1;

private:
    TwoWire& wireFor(int strip) { return stripBus(strip) == 0 ? Wire : Wire1; }
    int attnFor(int strip)      { return stripBus(strip) == 0 ? ATTN0 : ATTN1; }
    void attn(int strip, bool hi) { digitalWrite(attnFor(strip), hi ? HIGH : LOW); }

    bool writeCmd(int strip, const uint8_t* data, int len, bool demote = true);
    bool readFrame(int strip, uint8_t op, uint8_t sub, uint16_t chunk, uint8_t* frame, int cap);

    static constexpr int PIN_I2C0_SDA = 21, PIN_I2C0_SCL = 22, ATTN0 = 23;
    static constexpr int PIN_I2C1_SDA = 32, PIN_I2C1_SCL = 33, ATTN1 = 25;
    static constexpr uint32_t I2C_HZ = 400000;
    static constexpr int MAX_RETRY = 3;

    bool online_[N_STRIPS] = {false};
};

}  // namespace chimera
