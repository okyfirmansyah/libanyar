# LibAnyar — WiFi Channel Analyzer Example

A real-time WiFi spectrum analyzer that demonstrates **native Linux nl80211 WiFi scanning** combined with **zero-copy SharedBuffer WebGL rendering**. The C++ backend uses the kernel's netlink API (via libnl-3) to enumerate nearby access points, renders a 2.4/5 GHz channel occupation bitmap into POSIX shared memory, and delivers it to the Svelte frontend at ~10 fps for WebGL display — all without a single byte copied through the JavaScript bridge.

## Features

- **nl80211 passive scanning** — Reads the kernel's BSS cache via generic netlink; no root privileges required for passive mode
- **nl80211 active scanning** — Triggers a full channel sweep (requires `CAP_NET_ADMIN` / root); results arrive asynchronously and are picked up by the next scan cycle
- **Real-time spectrum bitmap** — C++ renders a 800×400 RGBA channel occupation chart directly into a `SharedBufferPool` (2 slots, POSIX mmap)
- **Zero-copy WebGL display** — `anyar-shm://` fetch delivers the bitmap to `texImage2D` without serialisation overhead
- **Network table** — Sortable by SSID, channel, or signal strength; colour-coded signal bars (green / yellow / red)
- **Associated AP highlight** — The currently connected network is visually distinguished in the table
- **Auto-detection** — Automatically finds the first wireless interface via `/sys/class/net/*/wireless`

## Architecture

```
┌────────────────────────────────────────────────────────┐
│  Svelte 5 + Vite Frontend                              │
│                                                        │
│  ┌────────────────────────────────────────────────────┐ │
│  │ App.svelte (layout + state orchestration)          │ │
│  │  ┌───────────────────┐  ┌───────────────────────┐  │ │
│  │  │ Spectrum Canvas    │  │ Network Table         │  │ │
│  │  │ (WebGL RGBA)       │  │ (sortable, colored)   │  │ │
│  │  └──────┬─────────────┘  └──────┬────────────────┘  │ │
│  │         │                       │                   │ │
│  │  SharedBuffer IPC        @libanyar/api              │ │
│  │  (anyar-shm:// fetch)   (invoke / listen)           │ │
│  └─────────┼───────────────────────┼───────────────────┘ │
└────────────┼───────────────────────┼─────────────────────┘
             │                       │
     anyar-shm:// fetch        HTTP / JSON IPC
   + buffer:ready events             │
             │                       │
┌────────────┴───────────────────────┴─────────────────────┐
│  C++ Backend (LibAnyar Core + WifiPlugin)                 │
│                                                           │
│  WifiPlugin (libnl-3 / nl80211)                           │
│  ├── wifi:start        — begin periodic passive scanning  │
│  ├── wifi:stop         — stop scanning                    │
│  ├── wifi:scan-once    — trigger a single scan cycle      │
│  ├── wifi:active-scan  — trigger full channel sweep       │
│  │                      (requires CAP_NET_ADMIN)          │
│  └── scan_loop()       — periodic scan + render cycle     │
│       ├── nl80211_scan()          — read BSS cache        │
│       ├── render_spectrum()       — draw RGBA bitmap      │
│       └── SharedBufferPool write  — emit buffer:ready     │
│                                                           │
│  SharedBufferPool "wifi-spectrum" (2 slots, 800×400 RGBA) │
│  Built-in: EventBus, HTTP + WebSocket IPC                 │
└───────────────────────────────────────────────────────────┘
```

### IPC Commands

| Command | Direction | Payload | Description |
|---------|-----------|---------|-------------|
| `wifi:start` | JS → C++ | — | Start periodic passive scanning |
| `wifi:stop` | JS → C++ | — | Stop scanning |
| `wifi:scan-once` | JS → C++ | — | Trigger a single scan + render cycle |
| `wifi:active-scan` | JS → C++ | — | Trigger nl80211 active scan (requires root) |

### Events (C++ → JS via EventBus)

| Event | Payload | Description |
|-------|---------|-------------|
| `buffer:ready` | `{pool, name, url, size, metadata: {width, height, networks}}` | New spectrum bitmap available in shared memory |
| `wifi:status` | `{scanning, interface}` | Scanning state change |
| `wifi:active-scan-done` | `{ok, error?}` | Active scan completed or failed |

## Requirements

### System

- **Ubuntu 22.04+** (or compatible Linux with GTK 3, WebKitGTK 4.0)
- **GCC 11+** with C++17 support
- **CMake ≥ 3.16**
- **Node.js ≥ 18** and npm
- A **wireless network interface** (e.g. `wlp3s0`, `wlan0`)

### Libraries

LibAnyar core dependencies (Boost, libasyik, SOCI, WebKitGTK, etc.) must already be installed via the project's `scripts/setup-ubuntu.sh` or equivalent.

In addition, this example requires **libnl-3 netlink libraries** for nl80211 WiFi scanning:

```bash
sudo apt install -y \
  libnl-3-dev \
  libnl-genl-3-dev
```

Verify with:

```bash
pkg-config --modversion libnl-3.0 libnl-genl-3.0
```

Expected output (Ubuntu 22.04):

```
3.5.0
3.5.0
```

### Runtime Permissions

**Passive scanning** (reading the kernel BSS cache) works without elevated privileges — any normal user can run the application.

**Active scanning** (`⚡ Active Scan` button) requires `CAP_NET_ADMIN` capability because it instructs the radio to cycle through all channels. You can either:

1. Run the binary as root:
   ```bash
   sudo ./wifi_analyzer
   ```

2. Or grant the capability to the binary (preferred):
   ```bash
   sudo setcap cap_net_admin+ep ./wifi_analyzer
   ```

Without elevated privileges, the Active Scan button will report an error, but passive scanning continues to work normally.

## Build

### 1. Build the frontend

```bash
cd examples/wifi-analyzer/frontend
npm install
npm run build
```

This produces `frontend/dist/` which is copied into the build directory by CMake.

### 2. Build the C++ binary

From the project root:

```bash
cd build
cmake ..
make wifi_analyzer -j$(nproc)
```

The binary is at `build/examples/wifi-analyzer/wifi_analyzer`.

### 3. Run

```bash
cd build/examples/wifi-analyzer
./wifi_analyzer
```

> **Note (Snap VS Code users):** If running from a VS Code terminal installed via Snap, use the `run.sh` wrapper to clean GTK environment variables:
> ```bash
> cd build/examples/wifi-analyzer
> ../../../run.sh ./wifi_analyzer
> ```

## Usage

1. The application automatically detects the first wireless interface and starts passive scanning
2. The **spectrum canvas** shows a real-time channel occupation chart (2.4 GHz band) with signal strength curves
3. The **network table** lists detected access points — click column headers to sort by SSID, channel, or signal strength
4. Signal bars are colour-coded: 🟢 ≥ −50 dBm (strong), 🟡 ≥ −70 dBm (moderate), 🔴 < −70 dBm (weak)
5. Your currently connected network is highlighted with a cyan accent
6. Click **⚡ Active Scan** to trigger a full channel sweep (requires elevated privileges)

## Tech Stack

| Layer | Technology | Purpose |
|-------|-----------|---------|
| Frontend | Svelte 5 + Vite 5 | Component framework + bundler |
| Styling | Tailwind CSS 4 | Utility-first CSS |
| IPC | @libanyar/api | JS ↔ C++ command bridge + SharedBuffer IPC |
| Backend | LibAnyar Core | App framework, HTTP server, SharedBuffer, WebView |
| WiFi | libnl-3 / nl80211 | Linux kernel WiFi scanning API |
| UI Toolkit | GTK 3 / WebKitGTK | Native window + embedded browser |

## File Structure

```
examples/wifi-analyzer/
├── CMakeLists.txt              # CMake config (libnl-3 + anyar_core linking)
├── README.md                   # This file
├── src-cpp/
│   ├── main.cpp                # Application entry point
│   ├── wifi_plugin.h           # WifiPlugin class + data structs
│   └── wifi_plugin.cpp         # nl80211 implementation + spectrum renderer
└── frontend/
    ├── package.json            # NPM dependencies
    ├── vite.config.js          # Vite + Svelte + Tailwind config
    ├── svelte.config.js        # Svelte compiler options
    ├── index.html              # HTML entry point
    └── src/
        ├── main.js             # Svelte mount
        ├── app.css             # Global styles + dark theme
        └── App.svelte          # Main layout: spectrum canvas + network table
```
