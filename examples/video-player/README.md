# LibAnyar — Mini Video Player Example

A desktop video player that combines an **HTML/JS frontend** with **C++ multimedia processing** via FFmpeg. Instead of relying on the `<video>` element, this example decodes all video in C++. It then either draws the frames natively under the webview (**Pinhole**, the default) or delivers them through **SharedBuffer** to a **WebGL** canvas. A hidden `<audio>` element plays the soundtrack and acts as the master clock.

It is also the reference for LibAnyar's background-work primitives ([ADR-009](../../docs/decisions.md)): `run_blocking`, `BackgroundTask`, `FrameMailbox` and `serve_file`.

## Features

- **Native or WebGL rendering.** Pinhole mode publishes decoded frames to an `anyar::FrameMailbox`, and the GTK render callback draws the latest one with `draw_frame()`, letterboxed. WebGL mode copies frames into a per-session `SharedBufferPool` and the JS `FrameRenderer` draws them.
- **Nothing blocks the service thread.** Probing, decoding, waveform and bitrate analysis all run on the worker pool through `anyar::run_blocking()`, so IPC stays responsive (ping latency stays in single-digit milliseconds while a 5-minute 720p file is analysed).
- **One decode session per file.** The decode loop is an `anyar::BackgroundTask`. Opening another file stops and *joins* the old session before the new one starts, so the two can't overlap. Each file gets a fresh player component in the UI.
- **Audio-driven sync.** The `<audio>` element is the master clock, and `video:sync` feeds it to C++. Frames are pre-decoded into a small queue, shown when due, and dropped when late. Files without audio use a wall clock.
- **Frame-accurate, acknowledged seeks.** The frontend pauses audio and sends `video:seek {time, id}`. C++ decodes from the preceding keyframe to the exact target frame, shows it, and replies `video:seeked {id}`. Only then does audio resume from that time. Rapid clicks collapse into the newest seek. Demuxers that overshoot a backward seek (for example MPEG-TS) are handled with progressive back-off.
- **Media timeline origin.** Timestamps are measured from the container start time, as browsers do. Streams with a nonzero start time (for example MPEG-TS) stay in sync.
- **Any pixel format.** YUV420P, NV12, NV21, RGBA, BGRA, RGB24 and GRAY8 are copied as-is. Anything else (10-bit, 4:2:2, …) is converted to YUV420P. Odd frame sizes are supported.
- **Audio-only `<audio>` source.** On open, the soundtrack is stream-copied (no re-encode) into a temporary Matroska file in the background. `/video/stream` serves that, not the full video. WebKit's media pipeline then never touches the video track, and the element downloads about 2 MB instead of the whole file.
- **Seek-safe audio.** Each timeline seek starts a *fresh* audio pipeline at the target instead of a flushing seek on the running one. On audio devices with a large buffer (common for onboard ALSA), WebKitGTK's pipeline can otherwise wedge permanently after a seek. A watchdog also rebuilds the pipeline if the audio clock ever stops advancing for 3 s while playing.
- **Range streaming.** `/video/stream` uses `anyar::serve_file()`, which always answers the full requested range (WebKit's media loader requires it) but streams it in 256 KB chunks read off the service thread, so memory stays bounded.
- **Audio waveform.** Computed in C++ (decode, resample to 8 kHz mono, min/max peaks) and rendered with [wavesurfer.js](https://wavesurfer.xyz/).
- **Bitrate monitor.** Per-bucket video and audio bitrate computed in C++ and plotted with [uPlot](https://github.com/leeoniya/uPlot). Clicks, the cursor and the axis share one mapping from the plot area, and the waveform is padded to the same plot area so both timelines line up exactly.
- **Auto-hiding overlay panel.** The waveform and bitrate chart slide out of the way during playback.

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Svelte 5 + Vite frontend                                    │
│   App.svelte ── {#key file} ── VideoPlayer (clock + seeks)   │
│                                 ├─ <audio src=/video/stream> │  ← master clock
│                                 └─ Pinhole div | WebGL canvas│
│   Waveform (wavesurfer.js)   BitrateChart (uPlot)            │
└──────────┬───────────────────────┬───────────────────────────┘
   invoke / listen (native IPC)    │ HTTP Range (206, streamed)
┌──────────┴───────────────────────┴───────────────────────────┐
│  VideoPlugin (service thread: fibers only schedule work)     │
│   video:open ──► stop+join old BackgroundTask ─► probe ─►    │
│                  start new BackgroundTask (paused)           │
│   decode loop ── run_blocking(decode/seek/convert) ──► queue │
│                  ─► present when PTS ≤ audio clock           │
│   video:waveform / video:bitrate ── run_blocking (parallel)  │
│   /video/stream ── anyar::serve_file (worker-pool reads)     │
├──────────────────────────────────────────────────────────────┤
│  Worker pool (LibAsyik async): FFmpeg demux/decode/swscale   │
├──────────────────────────────────────────────────────────────┤
│  pinhole: FrameMailbox ─► GTK on_render ─► draw_frame (GL)   │
│  webgl:   per-session SharedBufferPool ─► buffer:ready ─► JS │
└──────────────────────────────────────────────────────────────┘
```

### IPC Commands

| Command | Fields | Description |
|---------|--------|-------------|
| `video:open` | `path` | Stop and join the previous session, probe the file, start a paused session and show the first frame |
| `video:close` | — | Stop the session and forget the file |
| `video:bitrate` | `step?` | Packet-level bitrate buckets (worker pool, cached per file) |
| `video:waveform` | `samples?` | Audio min/max peaks (worker pool, cached per file) |
| `video:play` / `video:pause` | — | Start or stop frame presentation |
| `video:seek` | `time`, `id` | Seek to the exact frame; acknowledged by `video:seeked` with the same `id` |
| `video:sync` | `time` | Audio clock update |
| `video:pool-release` | `name` | WebGL mode: release a SharedBuffer slot after rendering |
| `video:get-mode` | — | `pinhole` or `webgl` |

### Events (C++ → JS)

| Event | Payload | Description |
|-------|---------|-------------|
| `video:ready` | `{width, height, fps, format}` | First frame decoded |
| `video:seeked` | `{id, time, pts}` | Seek finished, target frame shown |
| `video:frame-pts` | `{pts}` | Pinhole mode: a preview frame was shown |
| `buffer:ready` | `{pool, name, url, size, metadata: {width, height, pts, format}}` | WebGL mode: a frame is in shared memory |
| `video:ended` | `{}` | End of stream for video-only files (with audio, `<audio>` `ended` is used) |
| `video:error` | `{message}` | Decode, seek or open failure |

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
./video_player                # default: native pinhole overlay (Phase 4g)
./video_player --mode=pinhole # explicit
./video_player --mode=webgl   # legacy SharedBuffer + WebGL canvas path
```

> **Note (Snap VS Code users):** If running from a VS Code terminal installed via Snap, use the `run.sh` wrapper to clean GTK environment variables:
> ```bash
> cd build/examples/video-player
> ../../../run.sh ./video_player
> ```

### Render modes

| Mode | Path | Notes |
|---|---|---|
| `pinhole` (default) | FFmpeg decode (worker pool) → `FrameMailbox` → `Pinhole::on_render` → `draw_frame()` on the native `GtkGLArea` | No JS in the hot path. The render callback holds a reference to the frame it draws, so the decoder can never overwrite or free it mid-upload. Falls back to canvas-2D if GL is unavailable (logged warning). |
| `webgl` | FFmpeg decode → per-session `SharedBufferPool` (`try_acquire_write`: drops frames rather than stalling) → `buffer:ready` → JS `fetchBuffer` → WebGL | Use this if the GL overlay is unsupported on your target. |

The mode is selected at startup; switching requires relaunching the binary. See [docs/pinhole-rendering.md](../../docs/pinhole-rendering.md) for the full Pinhole API and architecture.

## Usage

1. Click **Open File** to select a video file (MP4, WebM, MKV, AVI, MOV, OGG)
2. The video loads — a poster frame appears on the canvas
3. Click the **▶ play** button in the transport bar to start playback
4. The **audio waveform** appears in the bottom overlay panel — click to seek
5. Toggle the **Bitrate Monitor** button to show/hide the bitrate chart
6. The bitrate chart shows per-interval video and audio bitrate. Click anywhere in the plot to seek to exactly that time.
7. During playback, the overlay panel auto-hides after 2 seconds; move the mouse to the bottom of the window to reveal it

## Debugging and the seek stress test

- `VIDEO_PLAYER_DEBUG=1 ./video_player` prints a timestamped backend trace on stderr: seeks and their duration, audio-clock gaps and jumps, periodic status, and `STALL` lines when video stops being presented while playing.
- `test/run_seek_stress.sh <file> [seeks] [--headless] [--mode=webgl]` drives the **real UI**. It injects `test/seek_stress.js` via `VIDEO_PLAYER_TEST_SCRIPT`, opens the file, presses ▶ with a real (GDK) click, clicks random points on the timeline, and checks that the audio clock keeps advancing. The exit code is non-zero if playback stalled. The log path printed at the end holds the backend trace.
- To reproduce device-latency problems silently, use a PulseAudio null sink with a large stream latency:
  ```bash
  MOD=$(pactl load-module module-null-sink sink_name=anyar_test)
  PULSE_SINK=anyar_test PULSE_LATENCY_MSEC=2000 test/run_seek_stress.sh ~/video.mp4 20 --headless
  pactl unload-module $MOD
  ```
  Extra switches: `SEED=<n>`, `NO_WATCHDOG=true`, `MEASURE_START=true` (report how long audio takes to restart after each seek).

## Tech Stack

| Layer | Technology | Purpose |
|-------|-----------|---------|
| Frontend | Svelte 5 + Vite 5 | Component framework + bundler |
| Styling | Tailwind CSS 4 | Utility-first CSS |
| Waveform | wavesurfer.js 7 | Audio waveform rendering |
| Chart | uPlot 1.6 | Lightweight time-series chart |
| IPC | @libanyar/api | JS ↔ C++ command bridge + SharedBuffer IPC |
| Backend | LibAnyar Core | App framework, HTTP server, SharedBuffer, WebView |
| Multimedia | FFmpeg 4.x | Video decode, swscale → YUV420P fallback, bitrate analysis, audio decode |
| UI Toolkit | GTK 3 / WebKitGTK | Native window + embedded browser |

## File Structure

```
examples/video-player/
├── CMakeLists.txt              # CMake config (FFmpeg + anyar_core linking)
├── README.md                   # This file
├── src-cpp/
│   ├── main.cpp                # Application entry point
│   ├── video_plugin.h          # VideoPlugin class + data structs
│   └── video_plugin.cpp        # FFmpeg decode/analysis, decode session, seek protocol
└── frontend/
    ├── package.json            # NPM dependencies
    ├── vite.config.js          # Vite + Svelte + Tailwind config
    ├── svelte.config.js        # Svelte compiler options
    ├── index.html              # HTML entry point
    └── src/
        ├── main.js             # Svelte mount
        ├── app.css             # Global styles + dark theme
        ├── App.svelte          # Main layout, transport controls, auto-hide panel
        ├── VideoPlayer.svelte  # clock + seek protocol; WebGL renderer or Pinhole placeholder
        ├── Waveform.svelte     # wavesurfer.js wrapper
        └── BitrateChart.svelte # uPlot bitrate chart
```
