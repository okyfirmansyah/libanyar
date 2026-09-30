<script>
  /**
   * VideoPlayer — renders raw frames decoded by the C++ VideoPlugin and keeps
   * them in sync with a hidden <audio> element (the master clock).
   *
   * Render paths:
   *   pinhole  C++ draws into a native GL surface under this placeholder div;
   *            JS only drives the clock.
   *   webgl    buffer:ready → anyar-shm:// fetch → WebGL FrameRenderer.
   *
   * Seek protocol (keeps audio and video from drifting apart):
   *   1. pause audio and stop clock sync
   *   2. invoke video:seek { time, id }
   *   3. C++ decodes the exact target frame, shows it, emits video:seeked { id }
   *   4. on the newest id: audio.currentTime = time, resume audio + sync
   *   Rapid clicks just bump the id; only the newest seek resumes playback.
   *
   * One instance per opened file (the parent wraps it in {#key}).
   *
   * Data-flow:
   *   currentTime  (bindable, OUT)  playback position → parent
   *   duration     (bindable, OUT)  media duration → parent
   *   playing      (bindable, OUT)  transport state → parent
   *   seekTarget   (prop, IN)       { time, id } from timeline clicks
   *   videoInfo    (prop, IN)       probe result from video:open
   *   audioSrc     (prop, IN)       HTTP URL for audio playback
   */

  import { invoke, listen } from '@libanyar/api';
  import { fetchBuffer } from '@libanyar/api/modules/buffer';
  import { createFrameRenderer } from '@libanyar/api/modules/canvas';
  import { onDestroy } from 'svelte';

  let {
    currentTime = $bindable(0),
    duration    = $bindable(0),
    playing     = $bindable(false),
    seekTarget  = null,
    videoInfo   = null,
    audioSrc    = '',
    /** 'pinhole' (native overlay) or 'webgl' (SharedBuffer + WebGL canvas). */
    renderMode  = 'webgl',
    ontoggleplay = null,
    /** Called with a message when the backend reports a playback error. */
    onerror     = null,
  } = $props();

  let canvasEl  = $state(null);
  let audioEl   = $state(null);

  let hasAudio = $derived(!!videoInfo?.audioCodec);
  let hasVideo = $derived(!!videoInfo?.videoCodec);

  // Seek state — plain lets: read inside callbacks, never rendered.
  let seeking       = false;   // between video:seek and video:seeked
  let seekSeq       = 0;
  let activeSeekId  = 0;
  let activeSeekTime = 0;
  let seekFallback  = null;    // timer in case the backend never answers
  let appliedTargetId = 0;

  // Wall-clock timing (files without an audio stream)
  let playStartTime   = 0;     // performance.now() at play/resume/seek
  let playStartOffset = 0;     // media time at that moment

  let playRafId = null;

  // ── Backend events ─────────────────────────────────────────────────────
  $effect(() => {
    if (!videoInfo) return;
    const unlisten = [];

    unlisten.push(listen('video:seeked', (e) => finishSeek(e?.id, e?.time)));
    unlisten.push(listen('video:ended', () => {
      // Only emitted for video-only files; with audio, <audio> `ended` drives this.
      if (playing) stopPlayback(duration);
    }));
    unlisten.push(listen('video:error', (e) => {
      console.error('[VideoPlayer] backend error:', e?.message);
      onerror?.(e?.message || 'Playback error');
    }));

    if (renderMode === 'pinhole') {
      // C++ draws frames natively; preview frames report their PTS.
      unlisten.push(listen('video:frame-pts', (e) => {
        if (!playing && !seeking && typeof e?.pts === 'number') currentTime = e.pts;
      }));
    } else if (canvasEl) {
      unlisten.push(setupWebglRenderer());
    }

    return () => unlisten.forEach((u) => u());
  });

  /** WebGL path: render buffer:ready frames; ALWAYS release the slot. */
  function setupWebglRenderer() {
    /** @type {ReturnType<typeof createFrameRenderer> | null} */
    let r = null;
    let fmt = '', rw = 0, rh = 0;

    const unlistenBuffer = listen('buffer:ready', async (event) => {
      if (!event?.pool?.startsWith('video-frames')) return;
      try {
        const data = await fetchBuffer(event.url);
        const meta = event.metadata || {};
        const f = meta.format || 'rgba';
        if (!r || f !== fmt || meta.width !== rw || meta.height !== rh) {
          if (r) r.destroy();
          fmt = f; rw = meta.width; rh = meta.height;
          r = createFrameRenderer({ canvas: canvasEl, width: rw, height: rh, format: fmt });
        }
        r.drawFrame(data);
        if (!playing && !seeking && typeof meta.pts === 'number') currentTime = meta.pts;
      } catch (err) {
        console.error('[VideoPlayer] Frame fetch/render error:', err);
      } finally {
        invoke('video:pool-release', { name: event.name }).catch(() => {});
      }
    });

    return () => {
      unlistenBuffer();
      if (r) { r.destroy(); r = null; }
    };
  }

  // ── Clock loop: audio (or wall clock) → currentTime + video:sync ───────
  function mediaNow() {
    if (hasAudio && audioEl) return audioEl.currentTime;
    return playStartOffset + (performance.now() - playStartTime) / 1000;
  }

  function startPlayLoop() {
    stopPlayLoop();
    const tick = () => {
      if (playing && !seeking) {
        const t = mediaNow();
        currentTime = t;
        invoke('video:sync', { time: t }).catch(() => {});
        watchAudioClock(t);
      }
      playRafId = requestAnimationFrame(tick);
    };
    playRafId = requestAnimationFrame(tick);
  }

  // ── Audio stall watchdog (safety net) ──────────────────────────────────
  // If the audio clock still stops advancing while we are playing, rebuild
  // the pipeline at the stalled position.  The threshold must exceed a
  // normal (re)start on a high-latency device, which can take ~0.6 s.
  const STALL_MS = 3000;
  let clockT = -1, clockSince = 0, recovering = false;

  function resetAudioWatchdog() {
    clockT = -1; clockSince = performance.now();
  }

  function watchAudioClock(t) {
    if (!hasAudio || !audioEl || recovering || window.__VP_TEST?.noWatchdog) return;
    const now = performance.now();
    if (Math.abs(t - clockT) > 0.001 || audioEl.ended) {
      clockT = t; clockSince = now;
      return;
    }
    if (now - clockSince < STALL_MS) return;
    if (duration > 0 && t >= duration - 0.25) return;   // at the very end: ended is imminent
    recoverAudio(t);
  }

  async function recoverAudio(t) {
    recovering = true;
    window.__vpAudioRecoveries = (window.__vpAudioRecoveries || 0) + 1;
    console.warn(`[VideoPlayer] audio clock stalled at ${t.toFixed(2)}s — restarting the audio pipeline`);
    try {
      await restartAudioAt(t);
    } finally {
      clockSince = performance.now();   // give the recovery a full window
      recovering = false;
    }
  }

  function stopPlayLoop() {
    if (playRafId) {
      cancelAnimationFrame(playRafId);
      playRafId = null;
    }
  }

  function resetWallClock(t) {
    playStartOffset = t;
    playStartTime = performance.now();
  }

  // ── Transport ──────────────────────────────────────────────────────────
  function play() {
    if (duration > 0 && currentTime >= duration - 0.05) seek(0);   // replay from start
    playing = true;
    invoke('video:play').catch(() => {});
    startPlayLoop();
    if (!seeking) resumeAudioAt(currentTime);
  }

  function pause() {
    playing = false;
    invoke('video:pause').catch(() => {});
    if (hasAudio && audioEl) {
      audioEl.pause();
      currentTime = audioEl.currentTime;
    } else {
      currentTime = mediaNow();
    }
    stopPlayLoop();
  }

  /** Playback reached the end (or was stopped): park at `at`. */
  function stopPlayback(at) {
    playing = false;
    invoke('video:pause').catch(() => {});
    if (audioEl) audioEl.pause();
    stopPlayLoop();
    if (typeof at === 'number') currentTime = at;
  }

  /**
   * Tear down the element's media pipeline and start a fresh one at t.
   *
   * Why: a flushing seek on a <audio> element that has been playing for a
   * while can leave WebKitGTK's GStreamer pipeline stuck for good when the
   * audio device has a large buffer (reproduced with a 2 s PulseAudio stream
   * latency; onboard ALSA devices often have this).  A new pipeline that
   * starts at t never goes through that path.  See test/run_seek_stress.sh.
   */
  let restartSeq = 0;
  async function restartAudioAt(t) {
    const my = ++restartSeq;
    const src = audioEl.src;
    audioEl.removeAttribute('src');
    audioEl.load();
    audioEl.src = src;
    await new Promise((r) => audioEl.addEventListener('loadedmetadata', r, { once: true }));
    if (my !== restartSeq || !audioEl) return;   // superseded by a newer seek/restart
    audioEl.currentTime = t;
    if (playing && !seeking) await audioEl.play().catch(() => {});
  }

  function resumeAudioAt(t, fromSeek = false) {
    resetWallClock(t);
    resetAudioWatchdog();
    if (!hasAudio || !audioEl) return;
    // Timeline seeks start a FRESH audio pipeline at t instead of a flushing
    // seek on the running one: on high-latency audio devices WebKitGTK's
    // GStreamer pipeline can wedge permanently after a flushing seek (see
    // restartAudioAt).  The audio-only source is small, so this is cheap.
    if (fromSeek) { restartAudioAt(t); return; }
    // Re-seeking the element costs an HTTP range request — only when needed.
    if (Math.abs(audioEl.currentTime - t) > 0.05) audioEl.currentTime = t;
    if (playing) audioEl.play().catch(() => {});
  }

  function seek(time) {
    const t = Math.max(0, duration > 0 ? Math.min(time, duration) : time);
    currentTime = t;
    if (hasAudio && audioEl) audioEl.pause();   // hold audio until video is there

    if (!hasVideo) {                            // audio-only file: nothing to wait for
      seeking = false;
      resumeAudioAt(t);
      return;
    }

    seeking = true;
    activeSeekId = ++seekSeq;
    activeSeekTime = t;
    invoke('video:seek', { time: t, id: activeSeekId }).catch(() => {});

    // Never leave the player stuck if the backend cannot answer.
    if (seekFallback) clearTimeout(seekFallback);
    const id = activeSeekId;
    seekFallback = setTimeout(() => finishSeek(id, t), 2000);
  }

  function finishSeek(id, time) {
    if (!seeking || id !== activeSeekId) return;   // superseded by a newer seek
    seeking = false;
    if (seekFallback) { clearTimeout(seekFallback); seekFallback = null; }
    const t = typeof time === 'number' ? time : activeSeekTime;
    currentTime = t;
    resumeAudioAt(t, true);
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
    if (!st || !st.id || st.id === appliedTargetId) return;
    appliedTargetId = st.id;
    seek(st.time);
  });

  // ── Duration ───────────────────────────────────────────────────────────
  // Probe duration until (or unless) the <audio> element reports its own.
  $effect(() => {
    if (!hasAudio && videoInfo?.duration) duration = videoInfo.duration;
  });

  function onAudioLoaded() {
    if (audioEl && Number.isFinite(audioEl.duration) && audioEl.duration > 0) {
      duration = audioEl.duration;
    }
  }

  function onAudioEnded() {
    if (playing) stopPlayback(duration);
  }

  onDestroy(() => {
    stopPlayLoop();
    if (seekFallback) clearTimeout(seekFallback);
    if (audioEl) {
      audioEl.pause();
      audioEl.removeAttribute('src');   // abort the HTTP stream right away
      audioEl.load();
    }
  });
</script>

<div class="relative w-full h-full overflow-hidden" style="background: {renderMode === 'pinhole' ? 'transparent' : '#000'};">
  {#if renderMode === 'pinhole'}
    <!-- Native overlay placeholder. The Pinhole tracking JS positions a
         GtkGLArea exactly over this div. The GL widget is drawn UNDER a
         transparent webview, so HTML composites freely on top of the GL
         surface. The placeholder fills the full container at all times. -->
    <div
      data-anyar-pinhole="video"
      class="absolute inset-0"
      style="background: transparent;"
    ></div>
  {:else}
    <canvas
      bind:this={canvasEl}
      class="absolute inset-0 w-full h-full"
      style="object-fit: contain; background: #000;"
    ></canvas>
  {/if}

  <!-- Hidden audio element — the master clock (only when the file has audio) -->
  {#if hasAudio}
    <audio
      bind:this={audioEl}
      src={audioSrc}
      preload="auto"
      onloadedmetadata={onAudioLoaded}
      onended={onAudioEnded}
      style="display: none;"
    ></audio>
  {/if}
</div>
