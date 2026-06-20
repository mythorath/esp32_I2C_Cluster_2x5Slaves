// persist.h - durable append-only spool over fs::FS (LittleFS today, SD later).
//
// Implements docs/cluster-telemetry-and-persistence.md sec 5: the master spools
// its irreplaceable streams (fossil-record events; low-rate vitals snapshots) to
// flash so they survive Selis outages and master reboots, then drains them to
// Selis on reconnect. Two instances: events (precious, never evicted) and vitals
// (evictable oldest-first under disk pressure).
//
// Record framing: [0xE5][seq:u32 LE][len:u8][payload:len][crc8]. The CRC lets
// boot recovery detect and trim a torn final record after a brownout. Segments
// are capped (SEG_CAP) and deleted whole once fully acked (cheap reclaim).
#pragma once

#include <FS.h>
#include <stdint.h>

namespace chimera {

static constexpr uint8_t  REC_MAGIC = 0xE5;
static constexpr uint32_t SEG_CAP   = 65536;   // 64 KB segment cap

class LogStream {
public:
    typedef void (*ReplayCb)(uint32_t seq, const uint8_t* payload, uint8_t len, void* ctx);

    // dir e.g. "/ev" or "/vit". evictable: vitals may be dropped under pressure.
    bool begin(fs::FS& fs, const char* dir, bool evictable);

    uint32_t append(const uint8_t* payload, uint8_t len);   // returns assigned seq
    uint32_t highestSeq() const { return seqMax_; }
    uint32_t lastAcked() const { return lastAcked_; }

    void replaySince(uint32_t sinceSeq, ReplayCb cb, void* ctx);
    void ackUpTo(uint32_t seq);                  // advance + delete fully-acked segments
    void maybeEvict(size_t minFreeBytes);        // evictable streams only
    void clear();                                // wipe all segments + state (env reset)

private:
    void  saveState();
    void  loadState();
    void  segPath(int idx, char* out, int cap) const;
    int   firstSegIndex() const;                 // lowest existing segment (or -1)
    int   newestSegIndex() const;                // highest existing segment (or -1)
    uint32_t segMaxSeq(int idx) const;           // highest seq stored in a segment (0 if none)
    void  recoverTail();                         // CRC-trim a torn final record

    fs::FS*  fs_ = nullptr;
    char     dir_[24] = {0};
    bool     evictable_ = false;
    uint32_t seqMax_ = 0;
    uint32_t lastAcked_ = 0;
    int      curSeg_ = 1;
    uint32_t curSegBytes_ = 0;
};

}  // namespace chimera
