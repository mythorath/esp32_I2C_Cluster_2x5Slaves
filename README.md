# Chimera Lenia — distributed artificial life cluster

**Two hemispheres, one mind.** A continuous multi-species [Lenia](https://chimeralabs.art/research/lenia/) world runs across **11 ESP32 boards**: one master orchestrates **10 I2C slave strips** (two heterogeneous banks) that evolve, migrate across a seam, and stream telemetry to a Linux dashboard.

Educational / art project — not a miner. (The repo’s earlier SHA-256 mining firmware lives under `archive/`.)

| | |
|---|---|
| **Firmware (this repo)** | [github.com/mythorath/esp32_I2C_Cluster_2x5Slaves](https://github.com/mythorath/esp32_I2C_Cluster_2x5Slaves) |
| **Dashboard & Selis server** | [github.com/mythorath/Chimera_Lenia_Dashboard](https://github.com/mythorath/Chimera_Lenia_Dashboard) |

---

## What it does

- **Bank A (instinct)** — 5× ESP32-C3, fixed-point Lenia, fast reactive dynamics  
- **Bank B (memory)** — 5× ESP32-S3, float Lenia with temporal echo, calmer persistence  
- **Two species** — prey + predator on every strip, coupled kernels, signed growth  
- **Master** — generation barrier, halo routing, seam transcoding, island evolution, organism detection, ST7789 OLED, outbound telemetry to Selis  
- **Selis (Linux)** — ingest, SQLite fossil record, browser dashboard (LIVE WebGL + CINEMA GPU dream stream)

The cluster **runs fully autonomously** when Selis or WiFi is down; telemetry and the web UI are optional sinks.

---

## Hardware

| Role | Board | I2C bus | Addresses | Lenia |
|------|-------|---------|-----------|--------|
| Master | ESP32 WROOM DevKit | both | — | orchestrator |
| Bank B (memory) | ESP32-S3 SuperMini ×5 | **Bus 0** (SDA 21 / SCL 22) | `0x08`–`0x0C` | float32 |
| Bank A (instinct) | ESP32-C3 SuperMini ×5 | **Bus 1** (SDA 32 / SCL 33) | `0x18`–`0x1C` | fixed-point |

Master also drives a **240×240 ST7789** (software SPI) and optional event LEDs.

World geometry (v1): **128×400** torus (10 strips × 40 rows), downsampled to **64×200** for the dashboard field frame. See `chimera/lib/shared/protocol.h`.

---

## Repository layout

```
.
├── chimera/                    # PlatformIO project (all active firmware)
│   ├── platformio.ini          # env: master | node_c3 | node_s3
│   ├── lib/shared/             # Lenia core, genome, protocol, topology
│   ├── src/master/             # orchestrator + telemetry client
│   ├── src/node_c3/            # Bank A slaves (set NODE_INDEX 0..4 per board)
│   ├── src/node_s3/            # Bank B slaves (set NODE_INDEX 0..4 per board)
│   └── tools/sim/              # Python reference simulators (desktop validation)
├── docs/                       # Telemetry protocol, Selis integration
├── chimera-lenia-architecture.md
└── archive/                    # Legacy SHA-256 mining sketches (historical)
```

---

## Quick start (firmware)

### 1. Secrets

```powershell
cd chimera/src/master
copy secrets.example.h secrets.h
# Edit secrets.h — WiFi + SELIS_HOST (IP or mDNS name of the dashboard host)
```

`secrets.h` is git-ignored. Never commit credentials.

### 2. Build & flash (PlatformIO)

From `chimera/`:

```powershell
# Master (adjust COM port)
pio run -e master -t upload --upload-port COM7

# Each slave — set NODE_INDEX in main.cpp (or -DNODE_INDEX=n) before upload
pio run -e node_s3 -t upload --upload-port COMx   # Bank B, indices 0..4
pio run -e node_c3 -t upload --upload-port COMx   # Bank A, indices 0..4
```

Serial monitor: `pio device monitor -b 115200`

### 3. Selis dashboard

On the Linux host (see the [dashboard repo](https://github.com/mythorath/Chimera_Lenia_Dashboard)):

```bash
npm install && npm run build && npm start
# Ingest :8787  |  Dashboard :8080
# GPU dream (optional): cd dream && bash _setup.sh && bash _run.sh
```

Point `SELIS_HOST` in master `secrets.h` at that machine’s LAN IP. The master dials **out** to `ws://SELIS_HOST:8787/ingest`.

---

## Development

| Task | Command |
|------|---------|
| Desktop Lenia sanity check | `python chimera/tools/sim/multichannel_ref.py --animate` |
| Mock cluster → Selis (no hardware) | `npm run mock` (on dashboard host) |
| Master serial vitals | `[gen N] online=… mass=…` every ~250 ms |

Deep specs:

- [`chimera-lenia-architecture.md`](chimera-lenia-architecture.md) — design thesis, phases, biology  
- [`docs/cluster-telemetry-and-persistence.md`](docs/cluster-telemetry-and-persistence.md) — master ↔ Selis wire protocol, flash spool  
- [`docs/selis-server-and-dashboard.md`](docs/selis-server-and-dashboard.md) — server-side mirror of the protocol  

---

## Autonomy contract

1. **I2C generation loop never blocks on the network.**  
2. **Evolution, homeostasis, and OLED keep running** if Selis is offline.  
3. **Durable events + vitals snapshots** spool to flash and replay on reconnect.  
4. **Selis** archives, visualizes, and runs the GPU dream — it is not in the hot path.

---

## License / credits

Hobby research sculpture built on Lenia (Chimeralabs / Bert Chan et al.). Firmware and dashboard by the repo owner; use and adapt with attribution.
