# ESP32 I2C SHA-256 Mining Cluster (2 buses × 5 slaves)

> **⚠️ 2026 reality check — host your own stratum server.** When running a system
> like this in 2026, hosting your own stratum server is essential. No matter the
> coin being mined, to get the most out of your fun little cluster — and to get
> more than one share accepted every few hours — running your own stratum server
> is a must.

An ESP32 master coordinating **10 slave MCUs** over **two I2C buses** to run a
distributed SHA-256 Stratum miner, with a live ST7789 cyberpunk dashboard.

> Educational / hobby project. ESP32 hash rates are tiny — this is for learning
> Stratum, I2C coordination, and embedded UI, not for profit.

## Layout

| Sketch | Role |
| --- | --- |
| `Master/` | Master controller: WiFi, Stratum client, dual-bus I2C, ST7789 UI |
| `SingleSlave/` | Single-core slave worker |
| `DualCoreSlave/` | Dual-core slave worker |
| `I2C_Scanner_v3/` | Bus scanner utility |
| `I2C_Scanner_v4_LCD/` | Bus scanner with LCD output (pin verification) |

## Architecture

- **Bus 0** — 5× ESP32-S3 slaves (addresses `0x08`–`0x0C`)
- **Bus 1** — 5× ESP32-C3 slaves (addresses `0x18`–`0x1C`)
- Master splits the nonce range across all online slaves, polls status, and
  submits found shares to the pool.

## Setup

1. Open `Master/Master.ino` in the Arduino IDE (or PlatformIO).
2. Copy the secrets template and fill in your own values:
   ```sh
   cp Master/secrets.example.h Master/secrets.h
   ```
   `secrets.h` is git-ignored and holds your WiFi, pool, and BTC payout details.
3. Install libraries: `Adafruit GFX`, `Adafruit ST7789`, `ArduinoJson`.
4. Flash the master and each slave with its matching sketch/address.

## Security

Real WiFi credentials and wallet addresses live only in `Master/secrets.h`,
which is excluded from version control via `.gitignore`. Never commit that file.
