# LibAnyar Benchmarks

`anyar_bench` measures one real webview session end to end:

| Metric | How it is measured |
|---|---|
| Startup | `main()` → `on_window_ready` → `window:created` → first IPC from the page (`bench:ready`) |
| IPC latency | Sequential `bench:echo` round-trips from JS: native `__anyar_ipc__` (small + 64 KB payload) and HTTP fallback `POST /__anyar__/invoke` |
| SharedBuffer | `fetch()` + `arrayBuffer()` of a 1920×1080 RGBA (8.3 MB) buffer via `anyar-shm://` and via HTTP `GET /__anyar__/buffer/<name>` |
| Pinhole | C++ time inside `on_render` for `draw_image()` of a 1080p RGBA frame (CPU submit incl. texture upload, not GPU completion), plus interval between frames (240 frames via `request_redraw()` chaining) |
| SharedBuffer sweep | `anyar-shm://` at 64 KB / 1 MB / 33 MB, plus XHR vs `fetch()` — separates per-request from per-byte cost |
| SharedBuffer webext (prototype) | `window.__anyar_shm_read(name)` from `shm_webext/` — WebProcess extension, one memcpy into a JS `Uint8Array` |
| Memory | RSS of the app process + all descendants (WebKit web/network processes), read from `/proc` |

WebKit clamps `performance.now()` to ~1 ms, so sub-ms IPC samples are **batched** (each sample = batch wall time / batch size; batch 50 native small, 10 native 64 KB, 25 HTTP). Percentiles are across batches.

## Run

```bash
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DANYAR_BUILD_BENCHMARKS=ON \
      -DANYAR_BUILD_EXAMPLES=OFF -DANYAR_BUILD_CLI=OFF
cmake --build build-bench -j --target anyar_bench
cd build-bench/benchmarks && ../../run.sh ./anyar_bench --json result.json
```

Needs a display **and an unlocked session** — a locked/blanked screen stops compositor frame callbacks, so Pinhole reports `n/a` (`total_renders: 0`). Prints a summary table plus one `BENCH_RESULT {json}` line; exit code 0 on success. The window closes itself (~10 s). `--no-webext` skips loading the prototype extension.

## Baseline — 2026-09-24

AMD Ryzen 7 8845HS (Radeon 780M), 30 GB RAM, Ubuntu 22.04.5, Wayland session, WebKitGTK 2.50.4 (webkit2gtk-4.0), Release build. Three consecutive runs; run 1 was a cold start.

| Metric | Result | Target (roadmap 6.4 / 4g) | |
|---|---|---|---|
| Startup → window ready (warm) | ~137 ms | — | |
| Startup → first IPC from page (warm) | ~330 ms | < 500 ms | ✅ |
| Startup → first IPC (cold, first run after boot/build) | ~2.3 s | < 500 ms | ⚠️ ~2.1 s spent before the window exists |
| IPC native, small payload | p50 0.10–0.12 ms, p99 ≤ 0.20 ms | < 1 ms | ✅ |
| IPC native, 64 KB payload | p50 2.0 ms, p99 ~3 ms | — | ⚠️ ~30 MB/s JSON path; use SharedBuffer for bulk data |
| IPC HTTP fallback, small | p50 0.44–0.48 ms | — | ✅ |
| SharedBuffer 1080p via `anyar-shm://` | p50 21–22 ms (~370 MB/s) | ~1 ms (4f claim) | ❌ ~20× off the documented figure |
| SharedBuffer 1080p via HTTP | p50 24–25 ms | — | only ~15% slower than shm |
| Pinhole `draw_image` 1080p RGBA, CPU | p50 0.68 ms, p95 1.57 ms | < 0.2 ms | ❌ (upload dominated) |
| Pinhole frame interval | p50 16.6 ms (vsync, 60 fps) | 1 frame | ✅ |
| Memory, total RSS | ~620 MB (app 245 + WebProcess 317 + NetworkProcess 64) | — | app RSS includes the 8.3 MB buffer + GL/Pinhole |

## SharedBuffer fetch investigation — 2026-09-24

**Root cause.** `anyar-shm://` responses are produced by `WebKitURISchemeRequest` in the UI process, which reads the `GInputStream` into a fixed `std::array<uint8_t, 8192>` and calls `didReceiveData()` per chunk — one async read + one IPC message to the WebProcess per **8 KB** ([WebKitURISchemeRequest.cpp](https://github.com/WebKit/WebKit/blob/main/Source/WebKit/UIProcess/API/glib/WebKitURISchemeRequest.cpp)). A 1080p frame is ~1,000 round-trips. Not configurable from the embedder; our handler (`g_bytes_new_with_free_func` over the mmap) is already copy-free on our side.

| `anyar-shm://` size | p50 | ≈ |
|---|---|---|
| 64 KB | 0.35 ms | per-request floor |
| 1 MB | 2.8 ms | |
| 8.3 MB (1080p) | 21 ms | XHR identical (22 ms) → not a `fetch()` issue |
| 33 MB (4K) | 90 ms | linear ≈ 2.7 ms/MB ≈ 370 MB/s |

**Zero-copy is blocked by JSC.** An `ArrayBuffer` over external memory (`jsc_value_new_array_buffer` on the mmap) aborts the WebProcess: `FATAL: Disabling Primitive gigacage is forbidden`. JS array storage must live inside JSC's Gigacage.

**Prototype that works — one memcpy in the WebProcess** (`shm_webext/anyar_shm_webext.cpp`, loaded via `webkit_web_context_set_web_extensions_directory`, UI pid passed as init data): `shm_open` + cached `mmap` of `/anyar_<pid>_<name>`, `memcpy` into a JSC-allocated `Uint8Array`.

| Path, 1080p RGBA | p50 | |
|---|---|---|
| `anyar-shm://` | 21–23 ms | current |
| HTTP fallback | 25–28 ms | |
| **webext `__anyar_shm_read`** | **0.35–0.40 ms** (p95 ≤ 2.6 ms) | ~60× faster; content verified; sees new C++ writes on next read |
| webext, 33 MB (4K) | 15 ms | vs 90 ms via `anyar-shm://` |

Productizing it (not done) means: ship the `.so` with apps (DEB/AppImage/`--embed` builds), load it from `App` before the first webview, expose it in `@libanyar/api/buffer` (e.g. `readBuffer()` with `anyar-shm://`/HTTP fallback) and use it in `FrameRenderer`. WebKitGTK's WebProcess sandbox is opt-in for webkit2gtk-4.0; a sandboxed WebProcess would need `/dev/shm` access.

### Takeaways
- **Startup and small IPC are well within targets** once warm.
- **`anyar-shm://` is not zero-copy in practice**: ~21 ms per 1080p frame (WebKit's 8 KB-chunked URI-scheme IPC, see above) caps the SharedBuffer+WebGL path well under 1080p60. A WebProcess extension brings it to ~0.4 ms (prototype).
- **Pinhole is the only path that sustains 1080p60 here** (0.7 ms CPU per frame, vsync-locked), though above its 0.2 ms target; a PBO / persistent-mapped texture upload is the obvious next optimization.
- **Cold start** (~2.1 s before the window exists) is worth profiling separately (WebKit process spawn, GL/shader caches).
