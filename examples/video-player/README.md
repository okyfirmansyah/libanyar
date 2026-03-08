# LibAnyar — Mini Video Player Example

A desktop video player that demonstrates the **flexibility of HTML/JS frontend rendering** combined with the **power of C++ multimedia processing** via FFmpeg. Unlike browser-based players that rely on the `<video>` element, this example performs all video decoding in C++ and streams raw RGBA frames over WebSocket to a Canvas-based renderer — proving that even pixel-level rendering pipelines work seamlessly in the LibAnyar architecture.

## Features

- **Canvas-based video rendering** — C++ FFmpeg decode → swscale RGBA → WebSocket binary push → `canvas.putImageData()` at ~60 fps
- **Audio-driven synchronisation** — Hidden `<audio>` element acts as the master clock; a `requestAnimationFrame` loop sends sync messages to C++ so the pre-decoded frame pool dispatches the correct frame
- **Pre-decoded frame pool** — C++ maintains a circular buffer of 5 decoded frames, eliminating decode latency during playback
- **Audio waveform** — Pre-computed in C++ (FFmpeg decode → resample → peak extraction), rendered with [wavesurfer.js](https://wavesurfer.xyz/)
- **Bitrate monitoring** — Per-bucket video/audio bitrate analysis computed in C++ (FFmpeg packet iteration), visualised with [uPlot](https://github.com/leeoniya/uPlot)
- **Auto-hiding overlay panel** — Waveform and bitrate chart slide in/out during playback with smooth transitions; video frame always fills the available space
- **Debounced seeking** — Rapid seek clicks are coalesced (80 ms) to prevent decoder confusion from overlapping seek+decode cycles

## Architecture

```
┌────────────────────────────────────────────────────────┐
│  Svelte 5 + Vite Frontend                              │
│                                                        │
│  ┌────────────────────────────────────────────────────┐ │
│  │ App.svelte (layout + state orchestration)          │ │
│  │  ┌──────────────┐  ┌────────┐  ┌───────────────┐  │ │
│  │  │ VideoPlayer   │  │Waveform│  │ BitrateChart  │  │ │
│  │  │ (Canvas RGBA) │  │(ws.js) │  │ (uPlot)       │  │ │
│  │  └──────┬────────┘  └───┬────┘  └──────┬────────┘  │ │
│  │         │               │               │           │ │
│  │  WebSocket binary  @libanyar/api (invoke)           │ │
│  └─────────┼───────────────┼───────────────┼───────────┘ │
└────────────┼───────────────┼───────────────┼─────────────┘
             │               │               │
    WS /video/frames    HTTP/JSON IPC    HTTP /video/stream
             │               │               │
┌────────────┴───────────────┴───────────────┴─────────────┐
│  C++ Backend (LibAnyar Core + VideoPlugin)                │
│                                                           │
│  VideoPlugin (FFmpeg 4.x)                                 │
│  ├── video:open     — probe metadata, start decode thread │
│  ├── video:bitrate  — packet-level bitrate analysis       │
│  ├── video:waveform — audio decode + peak extraction      │
│  ├── video:close    — cleanup resources                   │
│  ├── /video/frames  — WS: binary RGBA frame push +       │
│  │                    play/pause/seek/sync commands        │
│  └── /video/stream  — HTTP: audio-only file streaming     │
│                       (used by <audio> element)           │
│                                                           │
│  Frame decode pipeline:                                   │
│    av_read_frame → avcodec_send/receive → sws_scale →     │
│    5-frame circular pool → WS binary dispatch             │
│                                                           │
│  Built-in: DialogPlugin (native GTK file picker)          │
└───────────────────────────────────────────────────────────┘
```

### Binary Frame Format (little-endian)

| Offset | Type    | Field        |
|--------|---------|--------------|
| 0–3    | uint32  | width        |
| 4–7    | uint32  | height       |
| 8–15   | float64 | pts (seconds)|
| 16–19  | uint32  | frame_number |
| 20+    | uint8[] | RGBA pixels  |

### WebSocket Commands (JSON text frames, `/video/frames`)

| Command | Direction | Fields | Description |
|---------|-----------|--------|-------------|
| `play`  | JS → C++  | —      | Start decode + dispatch loop |
| `pause` | JS → C++  | —      | Stop dispatch, hold position |
| `seek`  | JS → C++  | `time` | Seek to timestamp, flush pool |
| `sync`  | JS → C++  | `time` | Audio clock update for frame selection |
| `ready` | C++ → JS  | —      | First frame decoded, ready to play |
| `ended` | C++ → JS  | —      | End of stream reached |

## Requirements

### System

- **Ubuntu 22.04+** (or compatible Linux with GTK 3, WebKitGTK 4.0)
- **GCC 11+** with C++17 support
- **CMake ≥ 3.16**
- **Node.js ≥ 18** and npm

### Libraries

LibAnyar core dependencies (Boost, libasyik, SOCI, etc.) must already be installed. In addition, this example requires **FFmpeg development libraries**:

```bash
sudo apt install -y \
  libavformat-dev \
  libavcodec-dev \
  libswresample-dev \
  libswscale-dev \
  libavutil-dev
```

Verify with:

```bash
pkg-config --modversion libavformat libavcodec libswresample libswscale libavutil
```

Expected output (Ubuntu 22.04):

```
58.76.100
58.134.100
3.9.100
5.9.100
56.70.100
```

## Build

### 1. Build the frontend

```bash
cd examples/video-player/frontend
npm install
npm run build
```

This produces `frontend/dist/` which is copied into the build directory by CMake.

### 2. Build the C++ binary

From the project root:

```bash
cd build
cmake ..
make video_player -j$(nproc)
```

The binary is at `build/examples/video-player/video_player`.

### 3. Run

```bash
cd build/examples/video-player
./video_player
```

> **Note (Snap VS Code users):** If running from a VS Code terminal installed via Snap, use the `run.sh` wrapper to clean GTK environment variables:
> ```bash
> cd build/examples/video-player
> ../../../run.sh ./video_player
> ```

## Usage

1. Click **Open File** to select a video file (MP4, WebM, MKV, AVI, MOV, OGG)
2. The video loads — a poster frame appears on the canvas
3. Click the **▶ play** button in the transport bar to start playback
4. The **audio waveform** appears in the bottom overlay panel — click to seek
5. Toggle the **Bitrate Monitor** button to show/hide the bitrate chart
6. The bitrate chart shows per-interval video (purple) and audio (cyan) bitrate — click to seek
7. During playback, the overlay panel auto-hides after 2 seconds; move the mouse to the bottom of the window to reveal it

## Tech Stack

| Layer | Technology | Purpose |
|-------|-----------|---------|
| Frontend | Svelte 5 + Vite 5 | Component framework + bundler |
| Styling | Tailwind CSS 4 | Utility-first CSS |
| Waveform | wavesurfer.js 7 | Audio waveform rendering |
| Chart | uPlot 1.6 | Lightweight time-series chart |
| IPC | @libanyar/api | JS ↔ C++ command bridge |
| Backend | LibAnyar Core | App framework, HTTP/WS server, WebView |
| Multimedia | FFmpeg 4.x | Video decode, swscale RGBA, bitrate analysis, audio decode |
| UI Toolkit | GTK 3 / WebKitGTK | Native window + embedded browser |

## File Structure

```
examples/video-player/
├── CMakeLists.txt              # CMake config (FFmpeg + anyar_core linking)
├── README.md                   # This file
├── src-cpp/
│   ├── main.cpp                # Application entry point
│   ├── video_plugin.h          # VideoPlugin class + data structs
│   └── video_plugin.cpp        # FFmpeg implementation
└── frontend/
    ├── package.json            # NPM dependencies
    ├── vite.config.js          # Vite + Svelte + Tailwind config
    ├── svelte.config.js        # Svelte compiler options
    ├── index.html              # HTML entry point
    └── src/
        ├── main.js             # Svelte mount
        ├── app.css             # Global styles + dark theme
        ├── App.svelte          # Main layout, transport controls, auto-hide panel
        ├── VideoPlayer.svelte  # Canvas RGBA renderer + WebSocket frame receiver
        ├── Waveform.svelte     # wavesurfer.js wrapper
        └── BitrateChart.svelte # uPlot bitrate chart
```
