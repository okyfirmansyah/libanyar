# LibAnyar — Mini Video Player Example

A desktop video player that demonstrates the **flexibility of HTML/JS frontend rendering** combined with the **power of C++ multimedia processing** via FFmpeg. Unlike browser-based players that rely on the `<video>` element, this example performs all video decoding in C++ and delivers raw RGBA frames via **SharedBuffer** (POSIX shared memory, zero-copy on Linux) to a **WebGL** renderer — proving that even pixel-level rendering pipelines work seamlessly in the LibAnyar architecture.

## Features

- **WebGL video rendering** — C++ FFmpeg decode → swscale RGBA → SharedBufferPool (mmap) → `anyar-shm://` fetch (zero-copy) → WebGL `texImage2D` at ~60 fps
- **Audio-driven synchronisation** — Hidden `<audio>` element acts as the master clock; a `requestAnimationFrame` loop sends sync messages to C++ so the pre-decoded frame pool dispatches the correct frame
- **Pre-decoded frame pool** — C++ maintains a `SharedBufferPool` of 5 slots backed by POSIX shared memory, eliminating decode latency during playback
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
│  │  │ (WebGL RGBA)  │  │(ws.js) │  │ (uPlot)       │  │ │
│  │  └──────┬────────┘  └───┬────┘  └──────┬────────┘  │ │
│  │         │               │               │           │ │
│  │  SharedBuffer IPC   @libanyar/api (invoke/listen)   │ │
│  └─────────┼───────────────┼───────────────┼───────────┘ │
└────────────┼───────────────┼───────────────┼─────────────┘
             │               │               │
     anyar-shm:// fetch  HTTP/JSON IPC   HTTP /video/stream
   + buffer:ready events     │               │
             │               │               │
┌────────────┴───────────────┴───────────────┴─────────────┐
│  C++ Backend (LibAnyar Core + VideoPlugin)                │
│                                                           │
│  VideoPlugin (FFmpeg 4.x)                                 │
│  ├── video:open     — probe metadata, start decode thread │
│  ├── video:bitrate  — packet-level bitrate analysis       │
│  ├── video:waveform — audio decode + peak extraction      │
│  ├── video:close    — cleanup resources                   │
│  ├── video:play     — start/resume SharedBuffer delivery  │
│  ├── video:pause    — pause frame delivery                │
│  ├── video:seek     — seek + flush pool + re-decode       │
│  ├── video:sync     — audio clock update for frame select │
│  ├── video:pool-release — release buffer after render     │
│  └── /video/stream  — HTTP: audio-only file streaming     │
│                       (used by <audio> element)           │
│                                                           │
│  Frame decode pipeline:                                   │
│    av_read_frame → avcodec_send/receive → sws_scale →     │
│    SharedBufferPool (5 slots, mmap) → buffer:ready event  │
│    → anyar-shm:// zero-copy fetch → WebGL texImage2D     │
│                                                           │
│  Built-in: DialogPlugin (native GTK file picker)          │
└───────────────────────────────────────────────────────────┘
```

### IPC Commands

| Command | Direction | Fields | Description |
|---------|-----------|--------|-------------|
| `video:play` | JS → C++ | — | Start decode + SharedBuffer dispatch loop |
| `video:pause` | JS → C++ | — | Pause dispatch, hold position |
| `video:seek` | JS → C++ | `time` | Seek to timestamp, flush pool, re-decode |
| `video:sync` | JS → C++ | `time` | Audio clock update for frame selection |
| `video:pool-release` | JS → C++ | `name` | Release a SharedBuffer slot after rendering |

### Events (C++ → JS via EventBus)

| Event | Payload | Description |
|-------|---------|-------------|
| `buffer:ready` | `{pool, name, url, metadata: {width, height, pts, frame}}` | New RGBA frame available in shared memory |
| `video:ready` | `{width, height}` | First frame decoded, ready to play |
| `video:ended` | `{}` | End of stream reached |

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
| IPC | @libanyar/api | JS ↔ C++ command bridge + SharedBuffer IPC |
| Backend | LibAnyar Core | App framework, HTTP server, SharedBuffer, WebView |
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
        ├── VideoPlayer.svelte  # WebGL RGBA renderer + SharedBuffer frame receiver
        ├── Waveform.svelte     # wavesurfer.js wrapper
        └── BitrateChart.svelte # uPlot bitrate chart
```
