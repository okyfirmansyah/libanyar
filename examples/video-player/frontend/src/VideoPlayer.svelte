<script>
  /**
   * VideoPlayer — Canvas-based renderer for raw RGBA frames from C++ backend.
   *
   * Architecture:
   *   C++ FFmpeg decode → swscale RGBA → WebSocket binary push →
   *   JavaScript ArrayBuffer → ImageData → canvas.putImageData
   *
   * Data-flow:
   *   currentTime  (bindable, OUT)  frame PTS → parent
   *   duration     (bindable, OUT)  audio metadata → parent
   *   seekTarget   (prop, IN)       parent → WS seek command
   *   videoInfo    (prop, IN)       probe result from video:open
   *   audioSrc     (prop, IN)       HTTP URL for audio playback
   *
   * Binary frame format (little-endian):
   *   bytes  0–3   uint32  width
   *   bytes  4–7   uint32  height
   *   bytes  8–15  float64 pts (seconds)
   *   bytes 16–19  uint32  frame_number
   *   bytes 20+    uint8[] RGBA pixel data
   */

  import { getBaseUrl } from '@libanyar/api';

  let {
    currentTime = $bindable(0),
    duration    = $bindable(0),
    playing     = $bindable(false),
    connected   = $bindable(false),
    seekTarget  = null,
    videoInfo   = null,
    audioSrc    = '',
    ontoggleplay = null,
  } = $props();

  let canvasEl  = $state(null);
  let audioEl   = $state(null);
  let ws        = $state(null);

  // Track which seekTarget id we've already applied
  let appliedSeekId = 0;

  // Play loop: audio-drives timeline + sends sync to C++
  let playRafId = null;

  // Debounce seek: coalesce rapid clicks into a single seek command
  let seekTimer = null;
  let seekPending = null;

  // ── WebSocket connection (reconnects when videoInfo changes) ───────────
  $effect(() => {
    if (!videoInfo) return;

    const base = getBaseUrl().replace(/^http/, 'ws');
    const url  = base + '/video/frames';
    const socket = new WebSocket(url);
    socket.binaryType = 'arraybuffer';

    socket.onopen = () => {
      ws = socket;
      connected = true;
      playing = false;
    };

    socket.onmessage = (e) => {
      if (e.data instanceof ArrayBuffer) {
        renderFrame(e.data);
      } else {
        try {
          const evt = JSON.parse(e.data);
          if (evt.event === 'ended') {
            playing = false;
            if (audioEl) audioEl.pause();
            stopPlayLoop();
          } else if (evt.event === 'ready') {
            // First frame available — seek to 0 for poster
            socket.send(JSON.stringify({ cmd: 'seek', time: 0 }));
          }
        } catch { /* ignore malformed text */ }
      }
    };

    socket.onclose = () => {
      connected = false;
      ws = null;
      playing = false;
    };

    socket.onerror = () => {
      connected = false;
    };

    return () => {
      stopPlayLoop();
      if (seekTimer) { clearTimeout(seekTimer); seekTimer = null; }
      socket.close();
      ws = null;
      connected = false;
      playing = false;
    };
  });

  // ── Render a binary RGBA frame to the canvas ──────────────────────────
  function renderFrame(buffer) {
    if (!canvasEl || buffer.byteLength < 20) return;

    const view = new DataView(buffer);
    const w   = view.getUint32(0, true);
    const h   = view.getUint32(4, true);
    const pts = view.getFloat64(8, true);
    // const frameNum = view.getUint32(16, true);

    const expectedSize = 20 + w * h * 4;
    if (buffer.byteLength < expectedSize) return;

    if (canvasEl.width !== w || canvasEl.height !== h) {
      canvasEl.width  = w;
      canvasEl.height = h;
    }

    const ctx  = canvasEl.getContext('2d');
    const rgba = new Uint8ClampedArray(buffer, 20, w * h * 4);
    const img  = new ImageData(rgba, w, h);
    ctx.putImageData(img, 0, 0);

    // During playback the audio element is the master clock;
    // only update currentTime from frame PTS when paused (seek preview).
    if (!playing) {
      currentTime = pts;
    }
  }

  // ── Play loop: audio-driven timeline + sync messages to C++ ─────────
  //
  // When playing, a requestAnimationFrame loop:
  //   1. Drives the timeline display from audioEl.currentTime
  //   2. Sends { cmd:"sync", time } to C++ so the decode loop knows
  //      which pre-decoded frame to dispatch next.
  function startPlayLoop() {
    stopPlayLoop();
    function tick() {
      if (playing) {
        // Always read the best available time and send sync to C++,
        // even while the audio element is buffering after a distant
        // seek.  Without this, C++ starves for sync messages and
        // the video decode loop stalls.
        const t = (audioEl && !audioEl.paused)
          ? audioEl.currentTime
          : currentTime;          // fallback while buffering
        currentTime = t;
        if (ws && connected) {
          ws.send(JSON.stringify({ cmd: 'sync', time: t }));
        }
      }
      playRafId = requestAnimationFrame(tick);
    }
    playRafId = requestAnimationFrame(tick);
  }

  function stopPlayLoop() {
    if (playRafId) {
      cancelAnimationFrame(playRafId);
      playRafId = null;
    }
  }

  // ── Transport controls ────────────────────────────────────────────────
  function play() {
    if (!ws || !connected) return;
    ws.send(JSON.stringify({ cmd: 'play' }));
    playing = true;
    // Start the sync loop IMMEDIATELY so C++ gets audio_time_
    // updates right away — don't wait for the audio promise.
    startPlayLoop();
    if (audioEl) {
      audioEl.currentTime = currentTime;
      audioEl.play().catch(() => {});
    }
  }

  function pause() {
    if (!ws || !connected) return;
    ws.send(JSON.stringify({ cmd: 'pause' }));
    playing = false;
    if (audioEl) audioEl.pause();
    stopPlayLoop();
  }

  function seek(time) {
    if (!ws || !connected) return;

    // Update UI and audio immediately so the user sees the response,
    // but debounce the actual C++ seek command.  When clicks arrive
    // faster than ~80 ms, only the LAST target is sent, preventing
    // the decoder from being hammered with overlapping seek+fast-
    // forward cycles that cause mmco / reference-frame confusion.
    currentTime = time;
    if (audioEl) audioEl.currentTime = time;

    seekPending = time;
    if (seekTimer) clearTimeout(seekTimer);
    seekTimer = setTimeout(() => {
      seekTimer = null;
      const t = seekPending;
      seekPending = null;
      if (!ws || !connected) return;
      ws.send(JSON.stringify({ cmd: 'seek', time: t }));
      if (audioEl && playing) audioEl.play().catch(() => {});
      if (playing) {
        ws.send(JSON.stringify({ cmd: 'sync', time: t }));
      }
    }, 80);
  }

  function togglePlay() {
    if (playing) pause();
    else play();
  }

  // Expose togglePlay to parent via callback prop
  $effect(() => {
    if (ontoggleplay) ontoggleplay(togglePlay);
  });

  // ── External seek (waveform / chart click) ────────────────────────────
  $effect(() => {
    const st = seekTarget;
    if (!st || !st.id || st.id === appliedSeekId || !ws || !connected) return;
    appliedSeekId = st.id;
    seek(st.time);
  });

  // ── Audio metadata → duration ─────────────────────────────────────────
  function onAudioLoaded() {
    if (audioEl) {
      duration = audioEl.duration || 0;
    }
  }

  // ── Time formatting ───────────────────────────────────────────────────
  function fmtTime(s) {
    if (!s || isNaN(s)) return '0:00';
    const m = Math.floor(s / 60);
    const sec = Math.floor(s % 60);
    return `${m}:${sec.toString().padStart(2, '0')}`;
  }
</script>

<div class="relative w-full h-full overflow-hidden" style="background: #000;">
  <canvas
    bind:this={canvasEl}
    class="absolute inset-0 w-full h-full"
    style="object-fit: contain; background: #000;"
  ></canvas>

  <!-- Hidden audio element — plays audio from the HTTP stream endpoint -->
  <audio
    bind:this={audioEl}
    src={audioSrc}
    preload="auto"
    onloadedmetadata={onAudioLoaded}
    style="display: none;"
  ></audio>
</div>
