#pragma once

// ============================================================================
// SECRETS TEMPLATE
// ----------------------------------------------------------------------------
// 1. Copy this file to "secrets.h" (in the same Master/ folder).
// 2. Fill in your own WiFi, pool, and BTC payout details.
// 3. secrets.h is listed in .gitignore and will NOT be committed.
// ============================================================================

// --- WiFi ---
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// --- Stratum Pool ---
#define POOL_HOST       "public-pool.io"
#define POOL_PORT       3333

// --- Bitcoin payout / worker ---
#define BTC_ADDRESS     "YOUR_BTC_ADDRESS"
#define WORKER_NAME     "ESP32Cluster"
#define WORKER_PASS     "x"
