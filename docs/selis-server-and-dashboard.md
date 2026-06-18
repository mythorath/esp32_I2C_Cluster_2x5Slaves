# Selis Side — Ingest Server, Archive & Dashboard

**Scope:** the Linux server ("Selis") application that ingests telemetry from the
ESP32 cluster, archives it durably, and serves a rich web dashboard. Lives in this
repo under `dashboard/` and runs **on Selis** (developed on Windows in this
workspace, deployed to Linux).

Companion doc: [`cluster-telemetry-and-persistence.md`](./cluster-telemetry-and-persistence.md)
(the cluster side). The **wire protocol in §4 of that doc is canonical**; this doc
mirrors the parts Selis implements. Keep them in sync.

Parent spec: [`../chimera-lenia-architecture.md`](../chimera-lenia-architecture.md).

---

## 1. Role & Principles

Selis is the cluster's **presentation + memory + heavy-compute** layer. It is
strictly **optional to the cluster's operation**: the cluster keeps evolving and
spooling to flash when Selis is down (see cluster doc §1). Therefore Selis is
built to:

1. **Accept a single inbound WebSocket** from the master (master is the *client*;
   Selis is the *server*). One connection per cluster.
2. **Archive the durable streams** (fossil-record events + vitals time-series) to a
   real database — this is the permanent record the cluster only buffers.
3. **Drive the reconnect/replay handshake** so no durable record is lost across
   outages, and acks let the cluster reclaim its flash.
4. **Fan out to unlimited browsers** — the rich live + historical dashboard.
5. **Host heavy/optional compute** the ESP32 can't: timelapse recording, history
   scrubbing, LLM narration, and (future) a higher-fidelity organism detector.

Selis being down must never wedge the cluster; conversely, the cluster being down
should leave Selis serving the last-known state + full history gracefully.

---

## 2. Stack & Layout

**Stack: Node + TypeScript.** Frontend with Vite; backend a small Node service
(`ws` for the cluster socket and browser fan-out, plus an HTTP layer for the SPA +
REST history). Database: **SQLite** to start (single file, zero-ops, perfect for
one cluster; the fossil record is low-volume). Swap to Postgres only if/when
multiple clusters or heavy analytics demand it.

```
dashboard/
├─ package.json            # workspaces: server + web
├─ README.md               # run/deploy notes (mirrors §7)
├─ server/
│  ├─ src/
│  │  ├─ index.ts          # boot: HTTP + WS, config, mDNS advertise (selis.local)
│  │  ├─ ingest.ts         # inbound WS from master: protocol §4, handshake §5
│  │  ├─ store.ts          # SQLite: events, vitals, ack cursors, organisms
│  │  ├─ hub.ts            # browser fan-out (live field + vitals + events)
│  │  ├─ recorder.ts       # timelapse: field frames -> disk (video/PNG seq)
│  │  ├─ narrator.ts       # optional LLM narration from world state
│  │  ├─ detector.ts       # (future) richer organism detection from field
│  │  └─ rest.ts           # REST: history queries for the SPA
│  └─ data/                # sqlite db + recordings (gitignored)
└─ web/
   ├─ index.html
   └─ src/
      ├─ main.ts           # WS client to Selis hub; routing
      ├─ field.ts          # canvas renderer (bank-aware palette, seam line)
      ├─ vitals.ts         # live vitals + fitness landscape
      ├─ lineage.ts        # fossil-record / lineage tree view
      ├─ organisms.ts      # field guide cards (id, name, biome, age, trajectory)
      └─ history.ts        # scrubbable timeline over archived vitals/events
```

Add `dashboard/` to this repo so firmware and UI version together. `data/` is
gitignored.

---

## 3. Two Connection Roles (don't confuse them)

```
  ESP32 master ──(outbound WS, client)──▶  Selis :8787 /ingest   (ingest.ts)
                                                  │
                                                  ▼
                                              store.ts (SQLite)
                                                  │
  Browsers   ◀──(WS fan-out + HTTP SPA)──── Selis :8080         (hub.ts + rest.ts)
```

- **Ingest socket (`:8787/ingest`):** exactly one peer, the master. Selis is the
  server here even though the master initiated — this is what lets the cluster
  stay independent (it dials out; no broker).
- **Browser hub (`:8080`):** serves the SPA and a fan-out WS for any number of
  browsers. Browsers never talk to the cluster directly.

Ports are defaults; align `SELIS_PORT`/`SELIS_PATH` with the cluster's `secrets.h`
(cluster doc §8).

---

## 4. Protocol Selis Implements (mirror of cluster doc §4)

One inbound WebSocket from the master. **Binary** = field frames; **text** = JSON
envelopes keyed by `"t"`. Little-endian.

### 4.1 Receive (master → Selis)
- **Binary field frame** `[0xCA][w=64][h=200][w*h bytes]` (12803 B) — **T1
  ephemeral**: fan out to browsers, feed the recorder; do **not** archive every
  frame to the DB (timelapse goes to the recorder, not SQLite).
- `"vitals"` (T1 live) — fan out to browsers; not individually archived.
- `"snap"` (T2b, has `vseq`) — **archive** to `vitals` table; advance vitals ack.
- `"event"` (T2a, has `eseq`) — **archive** to `events` table; update derived
  `organisms`/lineage; advance events ack; fan out to browsers.
- `"hello"` (control) — master announces `firmware, nStrips, dsW, dsH, evSeqMax,
  vitSeqMax`. Selis replies (see §5).

### 4.2 Send (Selis → master)
- `"hello"` `{ ackEv, ackVit }` — on connect, Selis's last stored seqs (0 if new).
- `"ack"` `{ ev, vit }` — periodic durable cursor so the master reclaims flash.
- `"cmd"` `{ name, args }` — *(reserved, future)* operator commands.

### 4.3 Handshake (Selis's responsibilities)
1. On master connect + `hello`: look up `MAX(eseq)`, `MAX(vseq)` already stored →
   reply `hello {ackEv, ackVit}`.
2. Master replays all `event`/`snap` with seq greater than those acks, then goes
   live. Selis inserts them **idempotently** (`INSERT OR IGNORE` on seq) so a
   double-send is harmless.
3. Selis sends `ack {ev, vit}` periodically (e.g. every 2 s or every N records)
   with the highest contiguous seq durably committed.

> **Idempotency is mandatory:** seqs are the dedupe key. Never assume exactly-once
> delivery across reconnects.

---

## 5. Storage (SQLite schema)

```sql
-- durable fossil record (T2a)
CREATE TABLE events (
  eseq        INTEGER PRIMARY KEY,   -- monotonic, from master; dedupe key
  gen         INTEGER NOT NULL,
  kind        TEXT    NOT NULL,      -- birth|death|colonize|migration|mutate|wildcard
  from_strip  INTEGER, to_strip INTEGER,
  lineage_id  INTEGER, organism_id INTEGER,
  fitness     REAL,
  text        TEXT,
  rx_ts       INTEGER NOT NULL       -- server receive time (ms epoch)
);

-- vitals time-series (T2b)
CREATE TABLE vitals (
  vseq        INTEGER PRIMARY KEY,
  gen         INTEGER NOT NULL,
  mass REAL, activity REAL, entropy REAL,
  best_fitness REAL, best_strip INTEGER,
  coupling REAL, organisms_alive INTEGER,
  births INTEGER, deaths INTEGER, migrations INTEGER, seam_crossings INTEGER,
  online INTEGER,
  rx_ts       INTEGER NOT NULL
);

-- derived: current/known organisms (from events + detection)
CREATE TABLE organisms (
  organism_id INTEGER PRIMARY KEY,
  name TEXT, bank INTEGER,
  birth_gen INTEGER, death_gen INTEGER,
  last_fitness REAL, last_seen_ts INTEGER
);

-- ack cursors (what we've durably committed; sent back to master)
CREATE TABLE ack_state (
  stream TEXT PRIMARY KEY,           -- 'ev' | 'vit'
  acked  INTEGER NOT NULL
);
```

- Archive `snap`/`event` on receipt with `INSERT OR IGNORE`; bump `ack_state`.
- Recordings (timelapse) live on disk under `server/data/recordings/`, **not** in
  SQLite. Store a manifest row if you want them queryable.

---

## 6. Modules

- **`ingest.ts`** — the master socket: parse §4 frames, run §5 handshake, write to
  `store.ts`, forward T1 + new events to `hub.ts`.
- **`store.ts`** — SQLite access; idempotent inserts; ack cursors; history queries.
- **`hub.ts`** — browser WS fan-out. On a new browser: send a snapshot (latest
  field + vitals + recent events) then stream deltas. Decoupled from `ingest` so a
  cluster outage just freezes the live view, not the server.
- **`recorder.ts`** — consume T1 field frames → timelapse (e.g. PNG sequence or
  piped to ffmpeg). Off by default; toggle in config.
- **`narrator.ts`** *(optional)* — compose richer narration from world state via a
  local/remote LLM; the cluster's `NARRATOR_LLM_URL` can point here. Posts to the
  event feed and (optionally) Mastodon/Bluesky.
- **`detector.ts`** *(future, Option C)* — higher-fidelity organism detection from
  the field stream, *augmenting* (not replacing) the master's on-device detector.
- **`rest.ts`** — history endpoints for the SPA, e.g.:
  - `GET /api/vitals?from=&to=` — vitals time-series for charts.
  - `GET /api/events?from=&to=&kind=` — fossil-record slice.
  - `GET /api/organisms` — field-guide catalogue.
  - `GET /api/recordings` — timelapse manifest.

---

## 7. Dashboard (web/) — views

Builds on the existing canvas prototype (`chimera/tools/viz/index.html`) but as a
proper app:

- **Live field** — canvas, bank-aware palette (Bank A vs B colored differently),
  seam line between strip 4/5, upscaled from 64×200. (Port `paletteA/paletteB` +
  `drawField` from the prototype.)
- **World vitals** — gen, nodes online, mass, entropy, best fitness, coupling,
  organisms, births/deaths/migrations/seam crossings (live).
- **Fitness landscape** — per-strip bars, bank-colored (port `buildBars`/`updateBars`).
- **Fossil record / lineage** — event feed + a lineage tree (who colonized/migrated
  from where, across the seam). This is the durable-archive payoff.
- **Field guide** — organism cards: id, generated name, biome of origin, age,
  tracked trajectory, fitness.
- **History scrubber** — timeline over archived `vitals`/`events`; scrub to replay
  vitals + (if recorded) timelapse. The thing the ESP32 fundamentally can't do.
- **Connection state** — show cluster link (live / cluster offline / reconnecting)
  distinctly from the browser↔Selis link.

---

## 8. Dev & Deploy Workflow

- **Author here on Windows** (Node is cross-platform), commit `dashboard/` to this
  repo so it versions with firmware.
- **Run on Selis (Linux):** `git pull` (or `rsync`) then `npm ci && npm run build`
  and run the server (systemd unit / pm2 for always-on). Same-LAN dev: you can
  also edit directly on Selis over SSH.
- **mDNS:** advertise `selis.local` so the cluster finds Selis without a static IP;
  keep a static-IP fallback consistent with the cluster's `secrets.h`.
- **Firewall:** open `:8787` (ingest, LAN-only) and `:8080` (dashboard). Keep the
  ingest port LAN-restricted.

### Suggested config (`dashboard/server/.env` or `config.json`)
| Key | Default | Meaning |
|---|---|---|
| `INGEST_PORT` | `8787` | master WS ingest |
| `INGEST_PATH` | `/ingest` | master WS path |
| `HTTP_PORT` | `8080` | SPA + browser WS |
| `DB_PATH` | `./data/chimera.db` | SQLite file |
| `RECORD` | `false` | enable timelapse recorder |
| `ACK_INTERVAL_MS` | `2000` | how often to ack the master |
| `LLM_URL` | _(unset)_ | optional narrator backend |

---

## 9. Implementation Checklist (Selis side)

- [ ] `dashboard/` workspace scaffold (server + web, TypeScript, Vite).
- [ ] `store.ts` + schema (§5); idempotent `INSERT OR IGNORE`; ack cursors.
- [ ] `ingest.ts`: master WS server on `:8787/ingest`; parse §4; handshake §5;
      periodic `ack`.
- [ ] `hub.ts`: browser fan-out on `:8080`; new-client snapshot + deltas.
- [ ] `rest.ts`: vitals/events/organisms/recordings history endpoints.
- [ ] `web/`: port field canvas + vitals + fitness bars from the prototype;
      add lineage tree, field guide, history scrubber, dual connection-state.
- [ ] mDNS advertise `selis.local`; `.env`/config; systemd/pm2 unit.
- [ ] `recorder.ts` (optional, default off).
- [ ] `narrator.ts` / `detector.ts` (future).

---

## 10. Open Questions / Future

- **Operator commands** (`"cmd"` channel) — reseed strip, set coupling/genome from
  the dashboard. Reserved; design the auth story before enabling writes to the
  cluster.
- **Multi-cluster / Postgres** — only if you run more than one cluster or need
  heavy analytical queries.
- **Richer detection on Selis** (Option C) — augment, never replace, the master's
  detector; gated on streaming higher-fidelity field data (architecture §6/§11).
- **Frames-to-SD merge** — when the cluster gains an SD recorder, decide whether
  Selis pulls on-device timelapse to fill gaps recorded while it was offline.
