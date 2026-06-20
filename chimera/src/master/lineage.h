// lineage.h - append-only evolutionary fossil record (Phase 7d).
//
// Logs genome lineage + migration events: which biome a winning rule came from,
// when it crossed the seam, organism births/deaths. This is the "useless
// blockchain" idea repurposed usefully as the evolutionary record. Kept in a
// RAM ring (newest N events) and surfaced by the web viz as a fossil record.
#pragma once

#include <stdint.h>

namespace chimera {

enum LineageType : uint8_t {
    LIN_BIRTH = 0, LIN_DEATH, LIN_COLONIZE, LIN_MIGRATION, LIN_MUTATE, LIN_WILDCARD,
    LIN_RESET,   // operator/serial full-environment reset (whole world reseeded)
};

struct LineageEvent {
    uint32_t gen;
    uint8_t  type;
    int8_t   fromStrip;
    int8_t   toStrip;
    uint16_t lineageId;
    uint16_t organismId;
    float    fitness;
};

class Lineage {
public:
    static constexpr int CAP = 256;

    // Durable sink (telemetry spool + Selis). Called for every recorded event so
    // the durable T2 stream is exactly the fossil record, without touching every
    // call site. Set after begin().
    typedef void (*SinkFn)(const LineageEvent&, void*);
    void setSink(SinkFn fn, void* ctx) { sink_ = fn; sinkCtx_ = ctx; }

    void begin() { head_ = 0; count_ = 0; total_ = 0; }

    void record(uint8_t type, uint32_t gen, int fromStrip, int toStrip,
                uint16_t lineageId, uint16_t organismId = 0, float fitness = 0) {
        LineageEvent& e = ring_[head_];
        e.gen = gen; e.type = type; e.fromStrip = (int8_t)fromStrip; e.toStrip = (int8_t)toStrip;
        e.lineageId = lineageId; e.organismId = organismId; e.fitness = fitness;
        if (sink_) sink_(e, sinkCtx_);    // durable spool + Selis
        head_ = (head_ + 1) % CAP;
        if (count_ < CAP) count_++;
        total_++;
    }

    int count() const { return count_; }
    uint32_t total() const { return total_; }
    // i=0 is the most recent event.
    const LineageEvent& recent(int i) const {
        int idx = (head_ - 1 - i + CAP * 2) % CAP;
        return ring_[idx];
    }

    static const char* typeName(uint8_t t) {
        switch (t) {
            case LIN_BIRTH: return "birth";
            case LIN_DEATH: return "death";
            case LIN_COLONIZE: return "colonize";
            case LIN_MIGRATION: return "migration";
            case LIN_MUTATE: return "mutate";
            case LIN_WILDCARD: return "wildcard";
            case LIN_RESET: return "reset";
            default: return "?";
        }
    }

private:
    LineageEvent ring_[CAP];
    int head_ = 0, count_ = 0;
    uint32_t total_ = 0;
    SinkFn sink_ = nullptr;
    void* sinkCtx_ = nullptr;
};

}  // namespace chimera
