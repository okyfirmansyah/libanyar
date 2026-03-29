<script>
  /**
   * VideoPlayer — WebGL renderer for raw frames from C++ SharedBuffer.
   *
   * Architecture:
   *   C++ FFmpeg decode → [optional swscale] → SharedBufferPool (mmap) →
   *   anyar-shm:// fetch (zero-copy) → WebGL texImage2D → drawFrame
   *
   * When the decoded pixel format is directly supported by the WebGL
   * renderer (yuv420, nv12, nv21, rgba, rgb, grayscale), swscale is
   * skipped entirely — saving CPU and bandwidth.
   *
   * Data-flow:
   *   currentTime  (bindable, OUT)  frame PTS → parent
   *   duration     (bindable, OUT)  audio metadata → parent
   *   seekTarget   (prop, IN)       parent → video:seek command
   *   videoInfo    (prop, IN)       probe result from video:open
   *   audioSrc     (prop, IN)       HTTP URL for audio playback
   */

  import { invoke, listen } from '@libanyar/api';
  import { fetchBuffer } from '@libanyar/api/modules/buffer';
  import { createFrameRenderer } from '@libanyar/api/modules/canvas';

  let {
    currentTime = $bindable(0),
    duration    = $bindable(0),
    playing     = $bindable(false),
    seekTarget  = null,
    videoInfo   = null,
    audioSrc    = '',
    ontoggleplay = null,
  } = $props();

  let canvasEl  = $state(null);
  let audioEl   = $state(null);
  let renderer  = $state(null);

  // Detect whether the file has an audio stream
  let hasAudio = $derived(videoInfo && videoInfo.audioCodec && videoInfo.audioCodec.length > 0);

  // Wall-clock timing state (used when no audio stream)
  let playStartTime   = 0;   // performance.now() when play/resume started
  let playStartOffset = 0;   // video time at which play/resume started

  // Track which seekTarget id we've already applied
  let appliedSeekId = 0;

  // Play loop: audio-drives timeline + sends sync to C++
  let playRafId = null;

  // Debounce seek: coalesce rapid clicks into a single seek command
  let seekTimer = null;
  let seekPending = null;

  // ── Initialize WebGL renderer + event listeners ────────────────────────
  //
  // The renderer is NOT created immediately — we wait for the first
  // buffer:ready event so we know the actual pixel format (yuv420, rgba,
  // etc.).  If the format or resolution changes mid-stream the renderer
  // is destroyed and recreated automatically.
  $effect(() => {
    if (!videoInfo || !canvasEl) return;

    /** @type {ReturnType<typeof createFrameRenderer> | null} */
    let r = null;
    let currentFmt = '';
    let rw = videoInfo.width;
    let rh = videoInfo.height;

    // Listen for buffer:ready events from the video plugin's SharedBufferPool.
    // We handle fetch + render manually (instead of createBufferRenderer) so
    // that the finally block ALWAYS releases the buffer back to the pool,
    // even when the fetch or render fails — preventing pool starvation.
    const unlistenBuffer = listen('buffer:ready', async (event) => {
      if (event.pool !== 'video-frames') return;
      try {
        const data = await fetchBuffer(event.url);

        const meta = event.metadata;
        const fmt = (meta && meta.format) ? meta.format : 'rgba';
        const newW = (meta && typeof meta.width === 'number') ? meta.width : rw;
        const newH = (meta && typeof meta.height === 'number') ? meta.height : rh;

        // (Re)create renderer on first frame, format change, or resolution change
        if (!r || fmt !== currentFmt || newW !== rw || newH !== rh) {
          if (r) r.destroy();
          rw = newW;
          rh = newH;
          currentFmt = fmt;
          r = createFrameRenderer({
            canvas: canvasEl,
            width: rw,
            height: rh,
            format: currentFmt,
          });
          renderer = r;
        }

        r.drawFrame(data);

        // Update PTS for timeline (only when paused — during playback
        // the audio element is the master clock).
        if (meta && typeof meta.pts === 'number') {
          if (!playing) currentTime = meta.pts;
        }
      } catch (err) {
        console.error('[VideoPlayer] Frame fetch/render error:', err);
      } finally {
        // Always release the buffer back to the plugin's pool so the
        // C++ decode loop can reuse the slot.
        invoke('video:pool-release', { name: event.name }).catch(() => {});
      }
    });

    // Listen for video:ended event
    const unlistenEnded = listen('video:ended', () => {
      playing = false;
      if (audioEl) audioEl.pause();
      stopPlayLoop();
    });

    return () => {
      stopPlayLoop();
      if (seekTimer) { clearTimeout(seekTimer); seekTimer = null; }
      unlistenBuffer();
      unlistenEnded();
      if (r) { r.destroy(); r = null; }
      renderer = null;
    };
  });

  // ── Play loop: audio-driven timeline + sync messages to C++ ─────────
  //    When the file has no audio stream, uses wall-clock timing instead.
  function startPlayLoop() {
    stopPlayLoop();
    function tick() {
      if (playing) {
        let t;
        if (hasAudio && audioEl && !audioEl.paused) {
          // Audio-driven: audio element is the master clock
          t = audioEl.currentTime;
        } else if (!hasAudio) {
          // Wall-clock-driven: compute elapsed time since play/resume
          t = playStartOffset + (performance.now() - playStartTime) / 1000;
        } else {
          t = currentTime;
        }
        currentTime = t;
        invoke('video:sync', { time: t }).catch(() => {});
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
    invoke('video:play').catch(() => {});
    playing = true;
    // Set wall-clock reference for video-only mode
    playStartOffset = currentTime;
    playStartTime = performance.now();
    startPlayLoop();
    if (hasAudio && audioEl) {
      audioEl.currentTime = currentTime;
      audioEl.play().catch(() => {});
    }
  }

  function pause() {
    invoke('video:pause').catch(() => {});
    playing = false;
    if (hasAudio && audioEl) audioEl.pause();
    stopPlayLoop();
  }

  function seek(time) {
    currentTime = time;
    // Reset wall-clock reference so video-only mode continues from seek point
    playStartOffset = time;
    playStartTime = performance.now();
    if (hasAudio && audioEl) audioEl.currentTime = time;

    seekPending = time;
    if (seekTimer) clearTimeout(seekTimer);
    seekTimer = setTimeout(() => {
      seekTimer = null;
      const t = seekPending;
      seekPending = null;
      invoke('video:seek', { time: t }).catch(() => {});
      if (hasAudio && audioEl && playing) audioEl.play().catch(() => {});
      if (playing) {
        invoke('video:sync', { time: t }).catch(() => {});
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
    if (!st || !st.id || st.id === appliedSeekId) return;
    appliedSeekId = st.id;
    seek(st.time);
  });

  // ── Audio metadata → duration ─────────────────────────────────────────
  function onAudioLoaded() {
    if (audioEl && hasAudio) {
      duration = audioEl.duration || 0;
    }
  }

  // ── Fallback duration from probe when no audio ────────────────────────
  $effect(() => {
    if (!hasAudio && videoInfo && videoInfo.duration) {
      duration = videoInfo.duration;
    }
  });

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

  <!-- Hidden audio element — plays audio from the HTTP stream endpoint (only when audio stream exists) -->
  {#if hasAudio}
    <audio
      bind:this={audioEl}
      src={audioSrc}
      preload="auto"
      onloadedmetadata={onAudioLoaded}
      style="display: none;"
    ></audio>
  {/if}
</div>
