#pragma once
// ============================================================================
// SECRETS TEMPLATE - copy to secrets.h (same folder) and fill in. secrets.h is
// git-ignored (**/secrets.h). The UI now lives on the Selis server; the master
// dials OUT to Selis over one WebSocket (docs/cluster-telemetry-and-persistence.md).
// ============================================================================

// --- WiFi (required to reach Selis; without it the cluster runs headless) ---
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// --- Selis ingest server (the Linux dashboard host) ---
// SELIS_HOST may be an mDNS name ("selis.local", resolved at boot) or a static IP.
#define SELIS_HOST     "selis.local"
#define SELIS_PORT     8787
#define SELIS_PATH     "/ingest"
// Durable vitals-snapshot cadence (ms). 30s -> ~2 weeks runway on internal flash.
#define VITALS_SNAP_MS 30000

// --- Autonomous narrator (optional, Phase 8) ---
#define NARRATOR_ENABLED  0
#define MASTODON_HOST     "mastodon.social"
#define MASTODON_TOKEN    "YOUR_MASTODON_ACCESS_TOKEN"
#define NARRATOR_LLM_URL  ""
