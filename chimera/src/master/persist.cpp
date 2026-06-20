#include <Arduino.h>
#include <string.h>
#include "persist.h"
#include "crc.h"

namespace chimera {

// A record on disk: magic(1) seq(4) len(1) payload(len) crc8(1).
static constexpr int REC_HDR = 6;   // magic+seq+len
static constexpr int REC_OVH = REC_HDR + 1;  // + crc8

void LogStream::segPath(int idx, char* out, int cap) const {
    snprintf(out, cap, "%s/%06d.log", dir_, idx);
}

bool LogStream::begin(fs::FS& fs, const char* dir, bool evictable) {
    fs_ = &fs;
    strncpy(dir_, dir, sizeof(dir_) - 1);
    evictable_ = evictable;
    if (!fs_->exists(dir_)) fs_->mkdir(dir_);
    loadState();
    int newest = newestSegIndex();
    if (newest < 0) { curSeg_ = 1; curSegBytes_ = 0; }
    else { curSeg_ = newest; recoverTail(); }
    // If state was lost/behind, recompute seqMax from the newest segment.
    uint32_t segMax = segMaxSeq(curSeg_);
    if (segMax > seqMax_) seqMax_ = segMax;
    return true;
}

void LogStream::loadState() {
    char p[40]; snprintf(p, sizeof p, "%s/state", dir_);
    File f = fs_->open(p, FILE_READ);
    if (!f) return;
    uint32_t v[2] = {0, 0};
    if (f.read((uint8_t*)v, sizeof v) == (int)sizeof v) { seqMax_ = v[0]; lastAcked_ = v[1]; }
    f.close();
}

void LogStream::saveState() {
    char tmp[40], p[40];
    snprintf(tmp, sizeof tmp, "%s/state.tmp", dir_);
    snprintf(p, sizeof p, "%s/state", dir_);
    File f = fs_->open(tmp, FILE_WRITE);
    if (!f) return;
    uint32_t v[2] = {seqMax_, lastAcked_};
    f.write((const uint8_t*)v, sizeof v);
    f.close();
    fs_->remove(p);
    fs_->rename(tmp, p);
}

int LogStream::firstSegIndex() const {
    int best = -1;
    File d = fs_->open(dir_);
    if (!d) return -1;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
        const char* nm = strrchr(e.name(), '/'); nm = nm ? nm + 1 : e.name();
        int idx = atoi(nm);
        if (idx > 0 && (best < 0 || idx < best)) best = idx;
        e.close();
    }
    d.close();
    return best;
}

int LogStream::newestSegIndex() const {
    int best = -1;
    File d = fs_->open(dir_);
    if (!d) return -1;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
        const char* nm = strrchr(e.name(), '/'); nm = nm ? nm + 1 : e.name();
        int idx = atoi(nm);
        if (idx > best) best = idx;
        e.close();
    }
    d.close();
    return best;
}

uint32_t LogStream::segMaxSeq(int idx) const {
    char p[40]; segPath(idx, p, sizeof p);
    File f = fs_->open(p, FILE_READ);
    if (!f) return 0;
    uint32_t maxSeq = 0;
    uint8_t hdr[REC_HDR];
    while (f.read(hdr, REC_HDR) == REC_HDR) {
        if (hdr[0] != REC_MAGIC) break;
        uint32_t seq; memcpy(&seq, hdr + 1, 4);
        uint8_t len = hdr[5];
        uint8_t skip[256 + 1];
        if (f.read(skip, len + 1) != len + 1) break;   // payload + crc
        maxSeq = seq;
    }
    f.close();
    return maxSeq;
}

void LogStream::recoverTail() {
    // Parse the newest segment; keep only the valid prefix (CRC-checked). If the
    // final record is torn, rewrite the segment truncated to the valid length.
    char p[40]; segPath(curSeg_, p, sizeof p);
    File f = fs_->open(p, FILE_READ);
    if (!f) { curSegBytes_ = 0; return; }
    uint32_t valid = 0;
    uint8_t rec[REC_OVH + 256];
    while (true) {
        int n = f.read(rec, REC_HDR);
        if (n != REC_HDR || rec[0] != REC_MAGIC) break;
        uint8_t len = rec[5];
        if (f.read(rec + REC_HDR, len + 1) != len + 1) break;
        if (crc8(rec + 1, REC_HDR - 1 + len) != rec[REC_HDR + len]) break;  // torn -> stop
        valid += REC_OVH + len;
    }
    uint32_t total = f.size();
    f.close();
    curSegBytes_ = valid;
    if (valid < total) {
        // rewrite truncated: copy valid prefix to a temp, swap.
        File in = fs_->open(p, FILE_READ);
        char tmp[40]; snprintf(tmp, sizeof tmp, "%s/seg.tmp", dir_);
        File out = fs_->open(tmp, FILE_WRITE);
        if (in && out) {
            uint8_t buf[256];
            uint32_t left = valid;
            while (left) {
                int chunk = left > sizeof(buf) ? sizeof(buf) : (int)left;
                int r = in.read(buf, chunk);
                if (r <= 0) break;
                out.write(buf, r); left -= r;
            }
        }
        if (in) in.close();
        if (out) out.close();
        fs_->remove(p);
        fs_->rename(tmp, p);
    }
}

uint32_t LogStream::append(const uint8_t* payload, uint8_t len) {
    if (!fs_) return 0;   // spool never mounted (LittleFS begin failed) -> no-op
    seqMax_++;
    uint8_t rec[REC_OVH + 256];
    rec[0] = REC_MAGIC;
    memcpy(rec + 1, &seqMax_, 4);
    rec[5] = len;
    if (len) memcpy(rec + REC_HDR, payload, len);
    rec[REC_HDR + len] = crc8(rec + 1, REC_HDR - 1 + len);
    int recLen = REC_OVH + len;

    if (curSegBytes_ + recLen > SEG_CAP) { curSeg_++; curSegBytes_ = 0; }
    char p[40]; segPath(curSeg_, p, sizeof p);
    File f = fs_->open(p, FILE_APPEND);
    if (f) { f.write(rec, recLen); f.close(); curSegBytes_ += recLen; }
    saveState();
    return seqMax_;
}

void LogStream::replaySince(uint32_t sinceSeq, ReplayCb cb, void* ctx) {
    int first = firstSegIndex();
    if (first < 0) return;
    for (int idx = first; idx <= curSeg_; idx++) {
        char p[40]; segPath(idx, p, sizeof p);
        File f = fs_->open(p, FILE_READ);
        if (!f) continue;
        uint8_t rec[REC_OVH + 256];
        while (true) {
            if (f.read(rec, REC_HDR) != REC_HDR || rec[0] != REC_MAGIC) break;
            uint8_t len = rec[5];
            if (f.read(rec + REC_HDR, len + 1) != len + 1) break;
            if (crc8(rec + 1, REC_HDR - 1 + len) != rec[REC_HDR + len]) break;
            uint32_t seq; memcpy(&seq, rec + 1, 4);
            if (seq > sinceSeq) cb(seq, rec + REC_HDR, len, ctx);
        }
        f.close();
    }
}

void LogStream::ackUpTo(uint32_t seq) {
    if (seq > lastAcked_) lastAcked_ = seq;
    // delete any non-current segment whose highest seq is <= lastAcked_
    int first = firstSegIndex();
    if (first < 0) { saveState(); return; }
    for (int idx = first; idx < curSeg_; idx++) {
        char p[40]; segPath(idx, p, sizeof p);
        if (!fs_->exists(p)) continue;
        if (segMaxSeq(idx) <= lastAcked_) fs_->remove(p);
    }
    saveState();
}

void LogStream::clear() {
    if (!fs_) return;
    // Remove every segment file, then the state file, and restart numbering. Used
    // by a full environment reset with clear_history (a deliberate fresh start).
    File d = fs_->open(dir_);
    if (d) {
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
            const char* nm = strrchr(e.name(), '/'); nm = nm ? nm + 1 : e.name();
            if (atoi(nm) > 0) { char p[40]; segPath(atoi(nm), p, sizeof p); e.close(); fs_->remove(p); }
            else e.close();
        }
        d.close();
    }
    char st[40]; snprintf(st, sizeof st, "%s/state", dir_);
    fs_->remove(st);
    seqMax_ = 0; lastAcked_ = 0; curSeg_ = 1; curSegBytes_ = 0;
}

void LogStream::maybeEvict(size_t minFreeBytes) {
    if (!evictable_) return;
    // Best-effort: LittleFS exposes totalBytes/usedBytes on the global object,
    // but through fs::FS& we approximate by dropping the oldest segment when the
    // caller signals pressure (minFreeBytes==0 forces one eviction).
    int first = firstSegIndex();
    if (first < 0 || first >= curSeg_) return;
    char p[40]; segPath(first, p, sizeof p);
    fs_->remove(p);
}

}  // namespace chimera
