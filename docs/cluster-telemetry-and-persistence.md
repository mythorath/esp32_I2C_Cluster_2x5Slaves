# Cluster Side — Telemetry Client & Local Persistence

**Scope:** the ESP32 *master* firmware (`chimera/src/master/`). This document
specifies how the cluster offloads its dashboard/visualization to the Linux
server ("Selis") **without ever depending on it**, and how it persists its
durable record to local flash now (with an SD-card upgrade path later).

Companion doc: [`selis-server-and-dashboard.md`](./selis-server-and-dashboard.md)
(the server side). The two docs **share one wire protocol** — defined here in §4,
mirrored there. Keep them in sync.

Parent spec: [`../chimera-lenia-architecture.md`](../chimera-lenia-architecture.md).

---

## 1. Goals & Non-Negotiables

1. **Autonomy first.** The cluster must run, evolve, narrate to its OLED, and
   record its own history with Selis **powered off or unreachable**. Selis is an
   optional *sink + presentation layer*, never a dependency. The I2C generation
   loop must never block on the network.
2. **Offload presentation & heavy analysis to Selis.** Move web serving, the
   browser fan-out, rich visualization, timelapse recording, long-term archival,
   and (later) LLM narration / richer organism detection off the ESP32.
3. **Durable local record.** Survive Selis outages *and* master reboots/brownouts
   by spooling the irreplaceable data (fossil-record events + a low-rate vitals
   time-series) to local flash, then draining it to Selis on reconnect.
4. **SD-ready.** Storage is written against `fs::FS` so the SD card is a drop-in
   that raises capacity and unlocks frame/timelapse history — no rewrite.

### What stays on the master (latency-/bus-coupled, must be local)
- Generation barrier (READY-GPIO polled), `CMD_TICK` broadcast.
- Halo collect/route/transcode across the two A↔B seams.
- Homeostasis + island evolution (`evolution`).
- Lightweight organism detection on the 64×200 stitched field (`detector`) —
  keeps the OLED + autonomy alive even when Selis is gone. **Do not remove this**
  when adding a richer Selis-side detector; *augment*, don't replace.
- OLED / ST7789 "field guide".

### What moves to Selis
- HTTP/WebSocket serving + browser fan-out (delete `WebServer`/`WebSocketsServer`
  from the master — see §3).
- Full-res / pretty rendering, timelapse recording, history scrubbing.
- Long-term fossil-record database + analytics.
- LLM-heavy narration (`NARRATOR_LLM_URL` hook in `narrator.h`).
- (Future) parallel higher-fidelity organism detection.

---

## 2. Data Tiers

Telemetry is split by how much we care if it is lost. This split drives both the
transport (§4) and the persistence (§5) design.

| Tier | Data | Rate | On Selis-down | Persisted to flash? |
|---|---|---|---|---|
| **T1 — ephemeral** | Stitched field frame `[0xCA][w][h][64×200]`; live vitals | field ~every `SLOW_MS` (400 ms), vitals same | **Dropped.** No value filming when nobody watches; buffering 12.8 KB/frame would exhaust RAM. | No |
| **T2a — durable events** | Fossil record (`LineageEvent`): births, deaths, colonize, migration, mutate, wildcard, seam crossings | minutes apart (sparse) | **Spooled to flash**, replayed on reconnect | Yes — *precious*, never evicted |
| **T2b — vitals time-series** | Compact `VitalsSnap` (mass, entropy, fitness, counts…) | ~every 30 s (tunable) | **Spooled to flash**, replayed on reconnect | Yes — evictable under pressure |

**Field/timelapse frames are NOT persisted to flash** — they're the bulk data the
SD card is for (§7). Flash holds only the small, durable T2 streams.

---

## 3. Transport: master = outbound WebSocket *client*

**Decision: a single outbound WebSocket client from the master to Selis.** Chosen
over MQTT because an MQTT broker would itself run on Selis — making the cluster
depend on Selis being up, which violates §1. A direct outbound WS:

- Reuses the already-present `links2004/WebSockets` lib (client mode instead of
  server mode).
- Is a single connection (no per-browser fan-out burden on the ESP; Selis fans
  out to browsers).
- Auto-reconnects; "is it connected?" is exactly the gate for T1 drop-vs-send.

**Discovery:** use **mDNS** to resolve `selis.local` (configurable host in
`secrets.h`) so DHCP changes don't require reflashing. Fall back to a static IP
from `secrets.h` if mDNS fails.

**Master-side refactor of the existing web layer** (`webserver.{h,cpp}`):
- Remove `WebServer http(80)` and `WebSocketsServer ws(81)` and their handlers
  (`handleRoot`, `handleApi`, `INDEX_HTML` serving). The embedded `index_html.h`
  is retired (Selis serves the UI).
- Replace with a `TelemetryClient` that owns one `WebSocketsClient`, exposes the
  same producer methods the rest of the firmware already calls so the call sites
  in `main.cpp` barely change:
  - `broadcastField(const Stitch&)` → send T1 binary frame **iff connected**.
  - `broadcastVitals(const WorldVitals&)` → send T1 live vitals JSON **iff connected**.
  - `pushEvent(type, text)` → still feeds the narrator/log, now also a T2 event.
- Non-blocking: `loop()` pumps the WS client; sends are best-effort and return
  immediately whether or not connected.

> Keep WiFi bring-up exactly as today (`main.cpp` `web.begin(...)` guarded by
> `HAVE_SECRETS`; "running headless" path preserved). No WiFi/no Selis ⇒ headless
> autonomy, unchanged.

---

## 4. Wire Protocol (canonical — mirrored in the Selis doc)

One WebSocket connection. **Binary** frames carry the field; **text** frames are
JSON envelopes with a `"t"` discriminator. All multi-byte values little-endian
(both ISAs are LE).

### 4.1 Master → Selis

**Binary — field frame (T1, ephemeral, unchanged from today):**
```
[0]      0xCA  magic
[1]      w     (= DS_W  = 64)
[2]      h     (= DS_H  = 200)
[3..]    w*h bytes, row-major, 0..255 cell intensity (downsampled stitched field)
```
Total `3 + 64*200 = 12803` bytes. Bank A = rows 0..99, Bank B = rows 100..199;
seam between strip 4 and 5.

**Text JSON — message types:**

| `t` | Tier | Has seq? | Fields |
|---|---|---|---|
| `"vitals"` | T1 live | no | `gen, online, mass, activity, entropy, bestFitness, bestStrip, coupling, organisms, births, deaths, migrations, seamCrossings` |
| `"snap"` | T2b durable | `vseq` | same scalar set as `VitalsSnap` (§5.3) + `vseq` |
| `"event"` | T2a durable | `eseq` | `eseq, gen, kind` (string: birth/death/colonize/migration/mutate/wildcard), `fromStrip, toStrip, lineageId, organismId, fitness, text` |
| `"hello"` | control | no | master announces: `firmware`, `nStrips`, `dsW`, `dsH`, `evSeqMax`, `vitSeqMax` |

`eseq`/`vseq` are **monotonic per stream, persisted across reboots** (§5.4).

### 4.2 Selis → Master

| `t` | Fields | Meaning |
|---|---|---|
| `"hello"` | `ackEv`, `ackVit` | On (re)connect: "I have events ≤ ackEv and vitals ≤ ackVit." Triggers replay (§6). |
| `"ack"` | `ev`, `vit` | Periodic durable cursor. Master reclaims fully-acked segments. |
| `"cmd"` (optional, future) | `name`, `args` | Operator commands (reseed strip, set coupling…). Out of scope v1; reserved. |

### 4.3 Reconnect / replay handshake
1. Master connects → sends `hello` with its current `evSeqMax`/`vitSeqMax`.
2. Selis replies `hello` with `ackEv`/`ackVit` (its last stored seqs, or 0 if new).
3. Master replays from flash **all events with `eseq > ackEv`** and **all vitals
   with `vseq > ackVit`**, in order, then resumes live T1 + new T2.
4. Selis periodically sends `ack`; master advances `lastAckedSeq` and deletes
   fully-drained segments (§5.2).

---

## 5. Local Persistence ("the spool")

New master module: **`persist.{h,cpp}`**. A generic segmented append-only log over
`fs::FS&`, instantiated twice (events + vitals). Today it's mounted on `LittleFS`;
later on `SD` with no code change.

### 5.1 Why `fs::FS`
On arduino-esp32, both `LittleFS` and `SD`/`SD_MMC` implement the same `fs::FS`
interface. Writing the spool against `fs::FS&` means the SD upgrade is literally:
```cpp
spool.begin(LittleFS, "/fossil");  // today
spool.begin(SD,       "/fossil");  // after adding the card reader
```
`LittleFS.h` ships with the core — **no new `lib_deps`**. First boot:
`LittleFS.begin(/*formatOnFail=*/true)`.

### 5.2 On-disk layout
```
/fossil/
├─ state                # per-stream {seqMax, lastAcked}; rewritten atomically
├─ ev/000001.log ...    # T2a events  — PRECIOUS, never evicted
└─ vit/000001.log ...   # T2b vitals  — evictable oldest-first under pressure
```
- **Record framing:** `[magic:u8=0xE5][seq:u32][len:u8][payload:len][crc8]`.
  The CRC lets boot recovery detect and trim a torn final record after brownout.
- **Segments** capped at **64 KB**. Append to the newest; when Selis acks past a
  whole segment, **delete that segment** (cheap reclaim, no big-file rewrite,
  bounds flash wear).
- **Eviction policy (disk pressure):** if free space drops below a threshold,
  delete the **oldest `vit/` segment first**; **never delete `ev/` segments.**
  The fossil record is irreplaceable; vitals are a decimatable time-series.

### 5.3 Payloads
- **Events** = the existing `LineageEvent` (`lineage.h`), 16 B packed:
  `{gen:u32, type:u8, fromStrip:i8, toStrip:i8, lineageId:u16, organismId:u16, fitness:f32}`.
- **Vitals snapshot** = new packed `VitalsSnap`, ~36 B, a scalar subset of
  `WorldVitals` (`world_state.h`):
  ```cpp
  struct __attribute__((packed)) VitalsSnap {
      uint32_t gen;
      float    mass, activity, entropy;
      float    bestFitness;  int16_t bestStrip;
      float    coupling;
      uint16_t organismsAlive;
      uint32_t births, deaths, migrations, seamCrossings;
      uint8_t  online;
  };
  ```

### 5.4 Sequence numbers & boot recovery
- Each stream keeps a **monotonic `seq`** persisted in `/fossil/state`, so numbers
  never repeat across reboots and acked records are never re-sent.
- On boot: read `state`; scan the newest segment, CRC-trim a torn tail if present;
  restore `seqMax` and `lastAcked`. Keep the existing `Lineage` RAM ring
  (`CAP=256`) as the fast "recent events" view for the OLED/dashboard — the spool
  is the durable spillover behind it.

### 5.5 Threading / placement
- **All FS I/O off the hot path** — in the existing off-hot-path block
  (`main.cpp`, the `millis() - lastSlow > SLOW_MS` section). **Never** in the I2C
  generation loop.
- Batch-flush every few seconds (or on segment close) rather than fsync per
  record; events are sparse so this is cheap.

---

## 6. Capacity & Runway (default partition, today)

Default partition table → **~1.5 MB** filesystem on the 4 MB WROOM-32 (OTA
preserved — chosen to keep dual-app OTA). Vitals dominate volume; events are
sparse.

| Vitals cadence | Records/day | Runway (≈1.5 MB) |
|---|---|---|
| every 10 s | ~8,640 | ~4 days |
| **every 30 s (default)** | ~2,880 | **~13 days** |
| every 60 s | ~1,440 | ~26 days |

**Default: 30 s snapshot cadence** (a tunable constant, decoupled from the 400 ms
`SLOW_MS` cycle via a counter) ⇒ ~2 weeks fully disconnected. The SD card (§7)
removes this ceiling and adds frame/timelapse history.

---

## 7. SD-Card Upgrade Path (later)

When the SD reader is added:
1. Mount `SD`/`SD_MMC`; change `spool.begin(LittleFS,…)` → `spool.begin(SD,…)`.
2. Raise the vitals cadence (or make it generation-based) — capacity is no longer
   the constraint.
3. **New T1-to-SD recorder:** spool the 12.8 KB field frames to the card for local
   timelapse (the bulk data flash can't hold). Selis can pull/merge these on
   reconnect, or they stay as an on-device backup.
4. Consider a dedicated SPI bus / pins; document wiring in `platformio.ini` next
   to the existing bus/pin notes.

---

## 8. Config Knobs (proposed)

In `secrets.h` (network) and a new `config.h` or compile flags (behavior):

| Knob | Default | Meaning |
|---|---|---|
| `SELIS_HOST` | `"selis.local"` | mDNS name (or static IP) of the server |
| `SELIS_PORT` | `8787` | Selis WS ingest port (see Selis doc) |
| `SELIS_PATH` | `"/ingest"` | WS path |
| `VITALS_SNAP_MS` | `30000` | T2b snapshot cadence |
| `SPOOL_SEGMENT_BYTES` | `65536` | log segment cap |
| `SPOOL_MIN_FREE_BYTES` | `131072` | evict oldest `vit/` below this |
| `FIELD_BROADCAST` | on | T1 field send (can disable to save WiFi airtime) |

Network creds stay in `secrets.h` (already gitignored via `secrets.example.h`
pattern). Add `SELIS_*` there too.

---

## 9. Implementation Checklist (cluster side)

- [ ] **`persist.{h,cpp}`** — segmented `LogStream` over `fs::FS&`; `append`,
      `replaySince`, `ackUpTo`, `highestSeq`; `/fossil/state` bookkeeping; boot
      recovery (CRC-trim); vitals-first eviction.
- [ ] **`VitalsSnap`** struct (in `world_state.h` or `persist.h`) + a 30 s sampler
      in the `SLOW_MS` block.
- [ ] **Refactor `webserver.{h,cpp}` → `TelemetryClient`**: drop `WebServer` +
      `WebSocketsServer`; add one outbound `WebSocketsClient`; mDNS resolve
      `SELIS_HOST`; implement §4 send/recv + §6 handshake; keep `broadcastField`,
      `broadcastVitals`, `pushEvent` signatures so `main.cpp` call sites are stable.
- [ ] **Wire the spool into `main.cpp`**: append on every `lineage.record(...)`;
      sample vitals every `VITALS_SNAP_MS`; drain via the WS client on reconnect.
- [ ] **Retire `index_html.h`** (UI now lives on Selis).
- [ ] **`secrets.example.h`**: add `SELIS_HOST/PORT/PATH`.
- [ ] Verify the hot loop never touches FS or blocks on the socket.

---

## 10. Open Questions / Future

- Operator command channel (`"cmd"` in §4.2) — reseed/coupling/genome poke from
  the dashboard. Reserved, not in v1.
- Move organism detection to Selis (Option C): run a *parallel* richer detector on
  Selis from the streamed field; keep the master's lightweight one for autonomy.
  Revisit after frames-to-SD lands.
- Field frames over the network are downsampled 64×200 today; a higher-fidelity
  stream (full-res, or `CMD_GET_TILE` at full precision) is a stretch goal gated by
  I2C bandwidth (see architecture §6/§11).
