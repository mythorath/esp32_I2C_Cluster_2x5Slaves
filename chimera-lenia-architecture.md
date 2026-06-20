# Distributed Lenia Cluster — Architecture & Build Spec

**Working title:** `chimera-lenia` (two hemispheres, one mind — rename freely)

A self-evolving, self-narrating artificial-life sculpture. A continuous cellular
automaton (Lenia) is spread across 11 microcontrollers. The two banks are two
genetically and *physically* distinct biomes that exchange migrants across a seam.
The world tunes its own rules to stay alive and interesting, discovers its own
organisms, and narrates them.

This doc is written to hand to Cursor/VS Code as the project spec. Build it in the
phased order in §10.

---

## 1. The Novel Thesis (what makes this 1-of-1)

Most Lenia runs on one machine and is deterministic. Most island-model GAs are pure
software and homogeneous. This piece is novel on three axes simultaneously:

1. **Hardware-rooted heterogeneous physics.** The two banks don't just *look*
   different — they compute under different number systems dictated by their actual
   silicon. Bank A (RISC-V C3) runs **fixed-point** Lenia; Bank B (Xtensa LX7 S3)
   runs **float32 + SIMD** Lenia with a PSRAM history buffer. Crossing the seam =
   transcoding between worlds.
2. **Self-tuning toward criticality.** A homeostatic controller watches the world's
   vital signs and nudges parameters to keep it at the edge of chaos. The system
   "wants" to stay alive.
3. **Rules that evolve in place.** Lenia parameters are a *genome*. Tiles compete on
   a "how interesting is my life" fitness; winners colonize neighbors and migrate
   across the seam (island model). Over hours/days the world discovers its own
   most-interesting physics.

Plus the experiential novelty: it **detects, names, and narrates its own creatures**,
and the copper transmission-line LEDs pulse on real internal events (halo exchange,
migration, birth/death).

---

## 2. Hardware Role Assignment

| Node | Chip | Role | Lenia mode |
|---|---|---|---|
| Master | ESP32 DevKit (LX6 x2, 520KB) | Orchestrator: generation barrier, halo routing, seam bridge, evolution engine, organism detection, OLED, web server, narrator | none (coordinator) |
| Bank A x5 | ESP32-C3 SuperMini (RISC-V @160MHz, 400KB) | "Instinct" hemisphere on **I2C bus 0** | **fixed-point** (uint16 state, LUT growth) |
| Bank B x5 | ESP32-S3 SuperMini (LX7 x2 @240MHz, 512KB +PSRAM) | "Memory" hemisphere on **I2C bus 1** | **float32 + SIMD**, PSRAM time-history |

The master is I2C master on **both** buses. Slaves never talk peer-to-peer; all
cross-tile data is master-mediated (this is forced by ESP32 I2C-slave limitations,
and it's also where the seam bridge and evolution hooks live).

```
                +-----------------------------+
                |      MASTER (ESP32 LX6)      |
                |  barrier · halo router       |
                |  seam bridge · evolution     |
                |  organism detect · OLED      |
                |  web viz · social narrator   |
                +---+---------------------+----+
        I2C bus 0   |                     |   I2C bus 1
        + 5 READY   |                     |   + 5 READY GPIO
   +----------------+---+           +-----+------------------+
   | BANK A — RISC-V C3 |  -- seam --|  BANK B — Xtensa S3    |
   | fixed-point biome  |  (master   |  float32+SIMD biome    |
   | A0 A1 A2 A3 A4     |   bridges) |  B0 B1 B2 B3 B4        |
   +--------------------+           +------------------------+
```

---

## 3. World Model & Topology

- The world is a **torus**, partitioned into **10 horizontal strips** (1 per slave).
  Strips, not tiles: each node has only **2 neighbors** (above/below), halving halo
  traffic vs 2D tiling.
- Strip order around the torus: `A0 A1 A2 A3 A4 | B0 B1 B2 B3 B4 |` wrapping back to
  A0. There are therefore **two seams** where Bank A meets Bank B (A4<->B0 and
  B4<->A0). Both are bridged + transcoded by the master.
- **Halo depth = kernel radius R.** Each node needs R boundary rows from each neighbor
  each generation. Keep strips tall relative to R so halo isn't most of the strip.
- **Uniform spatial grid across both banks** in v1 so strips align dimensionally at
  the seam. Heterogeneity lives in *math precision + parameters + history depth*, not
  resolution. (Higher-res S3 biome with up/downsampled seam = stretch goal, §11.)

**Suggested v1 sizing:** world `W=128 x H=400`, strips `128x40`, `R=13`.
- State buffer per node: `128x40 x2 buffers` = 20KB (float32) or 10KB (uint16) —
  trivial vs 400KB.
- Halo per neighbor row block: `128x13` cells. Sent as **uint8 quantized** (not
  float) -> ~1.6KB per neighbor (see §6 bandwidth math).

---

## 4. Exploiting the Heterogeneity

This is the conceptual spine — make it load-bearing, not cosmetic.

- **Bank A = fixed-point "instinct."** State `uint16` (0..65535 == 0..1). Growth
  function `G` precomputed as a 1024-entry **lookup table** indexed by quantized
  potential `U`. Convolution in `int32` accumulators. Visually blockier, quantized,
  fast. No FPU pressure.
- **Bank B = float32 "memory."** State `float32`, hardware FPU, **SIMD** (LX7 128-bit
  vector ops) for the convolution inner loop. PSRAM holds a **ring buffer of the last
  N fields** so Bank B can compute *temporal* fitness (autocorrelation-in-time -> "is
  this structure persistent but moving?"). Smoother, slower, deliberate.
- **Migration = transcoding.** When an organism patch crosses A->B, quantize->float;
  B->A, float->quantize (with dithering to avoid banding). The seam is literally a
  boundary between two number systems — capture this in captions.
- **Two hemispheres, variable coupling.** The master modulates how often the two banks
  exchange seam data. Sometimes tightly coupled (one integrated world), sometimes
  decoupled for many generations (each bank evolves independently), then reconnected —
  a "breathing" between integration and autonomy that the system controls itself (§5).

---

## 5. The Autonomy Layer (the novelty core)

Three controllers run on the master, escalating in timescale. Plain Lenia is the
substrate; these make it "do what it wants."

### 5a. Homeostasis (fast — every few generations)
Compute world **vital signs** from collected stats: total mass `M`, mass velocity
`dM/dt`, spatial entropy/heterogeneity, mean activity (cells changing). Define a target
"alive & interesting" band. If the world is dying (`M->0`) or saturating (`M->max`) or
frozen (activity->0), nudge global parameters (`mu`, `sigma`, `dt=1/T`) **toward the
edge of chaos**. Implement as a simple proportional controller on each parameter,
clamped to safe ranges. *Effect: the world fights for its own survival.*

### 5b. Island-model rule evolution (slow — every N generations / minutes)
- Each strip carries a **genome** `{R, T, mu, sigma, kernel beta-peaks, kernel
  mu_k/sigma_k}` (see §8).
- **Fitness** per strip = weighted combo of: persistence (temporal autocorrelation,
  from Bank B history), motion (center-of-mass drift != 0), structure (spatial entropy
  in a mid-band — neither empty nor full noise), and longevity of detected organisms.
- **Selection/migration:** periodically, high-fitness strips' genomes **colonize**
  lower-fitness neighbors (copy genome + reseed with a transplanted organism patch).
  Low-fitness strips get mutated genomes or fresh noise. The two banks are two
  **islands**; the master ferries migrant genomes across the seam at a controlled rate.
  Different mutation rates per bank -> genuinely divergent evolutionary trajectories.
- *Effect: the world discovers its own most-interesting rules over hours, and the two
  biomes diverge into distinct "lineages."*

### 5c. Self-cataloguing (slow — runs on stitched field)
On the assembled full field, the master runs **connected-component / blob detection**
to find coherent moving structures. Each gets an ID, a generated name, a birth time, a
tracked trajectory, and a death time. This "field guide" feeds the OLED and the
narrator (§9). *Effect: the system discovers and names its own creatures.*

### 5d. (Optional) Lineage fossil-record
Append-only log of genome lineage and migration events — which biome a winning rule
came from, when it crossed the seam. This is your earlier "useless blockchain" idea
repurposed *usefully* as the evolutionary record. Served by the web viz as a "fossil
record."

---

## 6. Communication Protocol

### Physical sync (works around ESP32 I2C-slave limits)
ESP32 I2C slaves **cannot clock-stretch**, so the master must never read before a slave
has data staged. Use a **per-node READY GPIO** (10 lines into the master — the DevKit
has the pins). A node raises READY when its generation is computed and halo+stats are
staged; master waits on all READY lines per bank before reading. (Fallback: one
wired-OR open-drain "all-ready" line per bank if pins are tight, losing per-node
laggard visibility.)

### Opcodes (master <-> node)
| Opcode | Dir | Payload | Notes |
|---|---|---|---|
| `CMD_TICK` | M->N | optional genome delta | compute one generation |
| `CMD_SET_GENOME` | M->N | full genome | migration/colonization |
| `CMD_SEND_HALO` | N->M | boundary rows (uint8, chunked) | node responds to read |
| `CMD_RECV_HALO` | M->N | neighbor boundary rows (chunked) | written before next TICK |
| `CMD_GET_STATS` | N->M | mass, activity, entropy, fitness | small, fixed-size |
| `CMD_GET_TILE` | N->M | full strip, downsampled uint8, chunked | for web viz, throttled |
| `CMD_MIGRATE_OUT` | N->M | organism patch + local genome | hand off across seam |
| `CMD_MIGRATE_IN` | M->N | transcoded patch | stamp into strip |
| `CMD_SEED` / `CMD_RESET` | M->N | pattern id or noise seed | reseed strip |

All multi-byte transfers are **chunked** into frames `[seq][len][payload][crc8]`
because Wire/I2C buffers are small (set `Wire.setBufferSize()` up; default Arduino is
tiny). Reassemble + CRC-check on receive; NACK -> retransmit the frame.

### The generation loop (master)
```
loop:
  for each node on both buses (parallel compute):
      send CMD_TICK (+ genome delta if evolving)
  wait until ALL READY GPIO high           # barrier — both banks compute simultaneously
  for each node: read CMD_SEND_HALO         # collect boundaries (uint8 quantized)
  route halos to neighbors;                 # incl. transcode across the two seams
  for each node: write CMD_RECV_HALO        # stage boundary for their next tick
  every K gens:  collect stats -> homeostasis (5a)
  every N gens:  evolution step (5b);  collect tiles -> stitch -> detect (5c) -> narrate
  pulse transmission-line LEDs on exchange / migration / birth events
```

### Bandwidth reality (be honest, then tune)
Halo as **uint8**: `128x13 = 1664 B` per neighbor. Per bank per generation =
`5 nodes x (2 send + 2 recv) x 1.6KB ~= 33KB`.
- @ 200kHz (current, ~20KB/s effective): **~1.6 s/gen** — conservative, robust on
  the existing harness/pull-ups; a deliberate step up from the legacy 100kHz cluster.
- @ 400kHz (~40KB/s effective): **~0.8 s/gen**.
- @ ~1MHz with good 2k-3k pull-ups (~100KB/s): **~0.3 s/gen** (~3 gens/sec).

This is a **slow, breathing** simulation — and that's *desirable* for watchable
swimming organisms. Knobs to speed up if wanted: shrink `R` (8 instead of 13), narrow
`W` (96), or send halos at 4-bit precision. Full-tile viz pulls (`~10x20KB`) are
downsampled uint8 and **throttled** to every few seconds, off the hot path.

---

## 7. Repo / File Structure (PlatformIO, multi-env)

PlatformIO in one project, **three build environments** sharing a `lib/`. Use
`build_src_filter` to compile the right `main` per env.

```
chimera-lenia/
├── platformio.ini              # envs: master, node_c3, node_s3
├── lib/
│   └── shared/                 # auto-included by all envs
│       ├── protocol.h          # opcodes, frame format, chunk/CRC helpers
│       ├── genome.h            # genome struct + (de)serialize + mutate
│       ├── lenia_core.h        # update step, templated <state_t, math policy>
│       ├── lenia_fixed.h       # uint16 + LUT growth + int32 conv
│       ├── lenia_float.h       # float32 + SIMD conv (LX7 intrinsics)
│       ├── topology.h          # strip map, neighbor + seam tables
│       └── crc.h
├── src/
│   ├── master/
│   │   ├── main.cpp            # generation loop, barrier
│   │   ├── bus_manager.{h,cpp} # dual TwoWire + READY-GPIO barrier
│   │   ├── halo_router.{h,cpp} # collect/route/transcode halos across seams
│   │   ├── stitch.{h,cpp}      # assemble strips -> full field
│   │   ├── detect.{h,cpp}      # blob detection, organism tracking + naming
│   │   ├── evolution.{h,cpp}   # homeostasis + island migration + fitness
│   │   ├── lineage.{h,cpp}     # optional fossil-record log
│   │   ├── display.{h,cpp}     # OLED field guide
│   │   ├── webserver.{h,cpp}   # WebSocket field stream + REST + fossil viz
│   │   └── narrator.{h,cpp}    # Mastodon/Bluesky posting (Lyuba-style)
│   ├── node_c3/
│   │   └── main.cpp            # bus0 slave, fixed-point biome
│   └── node_s3/
│       └── main.cpp            # bus1 slave, float biome, PSRAM history ring
├── tools/
│   ├── sim/                    # DESKTOP reference sim (Python or C) — validate FIRST
│   │   ├── lenia_ref.py        # find Orbium-like params before flashing anything
│   │   └── fitness_probe.py    # prototype the fitness metric offline
│   └── viz/
│       └── index.html          # web visualizer served by master (canvas + WS)
└── docs/
    └── ARCHITECTURE.md         # this file
```

`platformio.ini` sketch:
```ini
[env:master]
board = esp32dev
build_src_filter = +<master/> -<node_c3/> -<node_s3/>

[env:node_c3]
board = esp32-c3-devkitm-1
build_src_filter = +<node_c3/> -<master/> -<node_s3/>
build_flags = -DBIOME_FIXED

[env:node_s3]
board = esp32-s3-devkitc-1
build_src_filter = +<node_s3/> -<master/> -<node_c3/>
build_flags = -DBIOME_FLOAT -DBOARD_HAS_PSRAM
```

---

## 8. Lenia Math Reference (concrete — but validate on desktop first)

Standard Lenia (Chan 2019). State `A(x) in [0,1]` on a torus.

- **Kernel** `K`: radial, support radius `R`. Shell peaks `beta = [b1,...]`. Radial
  coord `r = dist/R in [0,1]`. Core bump `Ku(r) = exp(alpha(1 - 1/(4r(1-r))))`,
  `alpha=4`; or Gaussian `exp(-((r-mu_k)/sigma_k)^2/2)`. Multi-ring: split `r` into
  `len(beta)` rings scaled by `b_i`. **Normalize** so `sum(K) = 1`.
- **Potential** `U = K * A` (2D convolution, toroidal). This is the expensive step.
- **Growth** `G(u) = 2*exp(-((u-mu)/sigma)^2/2) - 1`, so `G in [-1,1]`.
- **Update** `A_new = clip(A + (1/T)*G(U), 0, 1)`.

**Orbium starter params** (the classic glider — validate/tune in `tools/sim`):
`R=13, T=10, mu=0.15, sigma=0.015`, single Gaussian ring `mu_k=0.5, sigma_k=0.15,
beta=[1]`.

**Fixed-point biome (C3):** `A` as `uint16`. Precompute `G` as a 1024-entry LUT over
quantized `U`. Convolution accumulates in `int32`, kernel as Q16 fixed-point,
renormalize after. Add small dither on the A->fixed transcode to avoid visible banding.

**Float biome (S3):** `A` as `float32`, FPU + SIMD inner conv loop. Naive
`(2R+1)^2`-tap convolution (~729 MACs/cell at R=13) is fine at strip sizes here
(~1.9M MACs/strip/gen -> tens of ms). Skip FFT — overkill. PSRAM ring buffer stores
last `N~=16` fields for temporal fitness.

---

## 9. Output / Experience Layer

- **OLED (master):** rotating "field guide" — newest named organism, its biome of
  origin, age, plus world vitals (mass, sync state, current best genome's fitness). The
  display *narrates discovery*, not raw numbers.
- **Web visualizer (master, WiFi):** WebSocket stream of the downsampled stitched field
  rendered to canvas — the Instagram money shot. Color Bank A and Bank B differently so
  the seam and migrations are visible. Side panel: live fitness landscape + fossil-record
  lineage tree.
- **Transmission-line LEDs:** addressable LEDs along the copper runs pulse on **real
  events** — halo crossing the seam, a migration, an organism birth/death, a
  leader-coupling change. Makes the sculpture's "thinking" physically visible.
- **Autonomous narrator (master):** posts to its own Mastodon/Bluesky — births,
  extinctions, seam crossings, "Bank A is dreaming of stripes tonight; Bank B
  disagrees." Lyuba (ringtailsoftware/lyuba) proves ESP32->Mastodon works; heavier text
  gen can offload to your NAS (Selis).

---

## 10. Phased Build Plan (do in this order)

1. **Desktop reference sim** (`tools/sim`). Get Orbium-like life on your laptop in
   Python/C. Lock in params + prototype the fitness metric. *Never debug Lenia on
   hardware first.*
2. **Single S3 node, standalone.** Float Lenia on one strip with wrap-around (no
   neighbors yet), dumping field over serial -> confirm life survives one chip.
3. **Single C3 node.** Port to fixed-point; confirm the LUT/int conv produces
   equivalent (blockier) life.
4. **Master + 2 nodes, one bus.** Implement the barrier (READY GPIO), `CMD_TICK`, and
   halo exchange between two strips. This de-risks the hardest part (synchronized halo
   over I2C). Validate against the desktop sim's two-strip output.
5. **One full bank (5 nodes, 1 bus).** Scale halo routing to a ring of 5. Add web viz
   + OLED.
6. **Both banks + master bridge.** Bring up bus 1, the two seams, and transcoding. Now
   it's a full heterogeneous world.
7. **Autonomy layers, in order:** homeostasis (5a) -> organism detection/naming (5c) ->
   island evolution (5b) -> optional fossil-record (5d) -> narrator.
8. **Show layer:** transmission-line LED events, polish the web viz, wire up social
   posting.

Each phase is independently demoable (and independently Instagrammable).

---

## 11. Risks, Knobs, Honest Constraints

- **I2C bandwidth** sets the frame rate (§6). Mitigations baked in: strips not tiles,
  uint8 halos, throttled viz. Accept ~0.3-1 gen/s as a *feature*. If it's still too
  slow, the escape hatch is ESP-NOW (wireless) — but that **sacrifices the wired-bus
  novelty**, so prefer wired and tune `R`/`W`/precision first.
- **No I2C slave clock-stretching** -> the READY-GPIO barrier is mandatory, not
  optional.
- **float64 is emulated** on both LX6/LX7 -> use float32 everywhere; never `double`.
- **Halo depth vs strip height:** keep `H_strip >~ 3R` or halo dominates the strip.
  (At `R=13`, `H=40` is fine; don't go below ~`H=30`.)
- **Evolution can collapse diversity** (one rule wins everywhere -> boring). Add a
  minimum mutation floor + occasional "wildcard" reseed, and keep per-bank mutation
  rates different so the islands stay distinct.
- **Higher-res S3 biome** (different grid than C3) is a real stretch goal: requires
  up/downsampling at the seam. Keep grids uniform in v1; revisit once §10.7 is stable.
- **Stitch/detect cost** on the master is the other compute hot spot; run it off the
  hot path (every N gens, on downsampled field).

---

## 12. Novelty Summary (vs prior art)

| Prior art | What they did | What we add |
|---|---|---|
| Lenia (Chan) / single-board CA | Continuous ALife, one machine, fixed rules | Physically distributed across 11 MCUs; rules self-evolve |
| Island-model GAs | Software, homogeneous islands | Islands = two real ISAs / number systems; Lenia organisms as the medium |
| ESP32 CA demos (Game of Life) | Single chip, discrete, static rules | Continuous, distributed, halo-exchanged, self-tuning |
| 3-body ESP-NOW cluster | Wireless, 3 nodes, physics not ALife | Wired dual-bus, 10 nodes, evolving ALife + heterogeneous biomes |
| Kuramoto firefly art | Wireless LED sync | Variable inter-hemisphere coupling as an evolutionary driver |

**The unclaimed combination:** distributed continuous artificial life, on real
heterogeneous microcontrollers, whose two number-system biomes co-evolve their own
rules toward criticality, and which discovers and narrates its own organisms. That
specific stack is what makes this a genuine 1-of-1.
