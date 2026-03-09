<script>
  import { invoke, getBaseUrl } from '@libanyar/api';
  import VideoPlayer from './VideoPlayer.svelte';
  import Waveform from './Waveform.svelte';
  import BitrateChart from './BitrateChart.svelte';

  let videoInfo = $state(null);
  let videoUrl = $state('');
  let bitrateData = $state(null);
  let waveformData = $state(null);
  let showBitrate = $state(true);
  let currentTime = $state(0);
  let duration = $state(0);
  let seekTarget = $state(null);   // { time, id } — set by waveform/chart clicks
  let loading = $state(false);
  let errorMsg = $state('');
  let gpuRenderer = $state('');

  // Detect GPU renderer used by WebGL (runs once)
  try {
    const c = document.createElement('canvas');
    const g = c.getContext('webgl') || c.getContext('experimental-webgl');
    if (g) {
      const dbg = g.getExtension('WEBGL_debug_renderer_info');
      gpuRenderer = dbg
        ? g.getParameter(dbg.UNMASKED_RENDERER_WEBGL)
        : g.getParameter(g.RENDERER);
    }
  } catch (_) { /* ignore */ }

  // Chart plot-area geometry (from BitrateChart) used to align waveform
  let chartPlotLeft = $state(0);
  let chartPlotWidth = $state(0);

  // Bottom panel auto-hide during playback
  let playing = $state(false);       // bound from VideoPlayer
  let vpTogglePlay = $state(null);   // function ref from VideoPlayer
  let panelVisible = $state(true);   // show/hide bottom panel
  let panelHideTimer = null;
  let contentEl = $state(null);      // ref for the content area
  let bottomPanelEl = $state(null);  // ref for measuring panel height

  function showPanel() {
    panelVisible = true;
    clearHideTimer();
  }

  function scheduleHidePanel() {
    clearHideTimer();
    if (!playing) return;
    panelHideTimer = setTimeout(() => {
      panelVisible = false;
    }, 2000);
  }

  function clearHideTimer() {
    if (panelHideTimer) {
      clearTimeout(panelHideTimer);
      panelHideTimer = null;
    }
  }

  function handleContentMouseMove(e) {
    if (!playing || !contentEl) { showPanel(); return; }
    const rect = contentEl.getBoundingClientRect();
    const bottomZone = rect.height * 0.4; // bottom 40% triggers show
    const yFromBottom = rect.bottom - e.clientY;
    if (yFromBottom <= bottomZone) {
      showPanel();
      // Will auto-hide after 2s
      scheduleHidePanel();
    }
  }

  function handleContentMouseLeave() {
    if (playing) scheduleHidePanel();
  }

  // When playback stops, always show panel
  $effect(() => {
    if (!playing) {
      showPanel();
    } else {
      scheduleHidePanel();
    }
  });

  async function openFile() {
    try {
      errorMsg = '';
      const paths = await invoke('dialog:open', {
        title: 'Select Video File',
        filters: [
          { name: 'Video Files', extensions: ['mp4', 'webm', 'mkv', 'avi', 'mov', 'ogg', 'flv', 'wmv'] }
        ]
      });
      if (!paths || paths.length === 0) return;

      loading = true;
      videoInfo = null;
      bitrateData = null;
      waveformData = null;
      currentTime = 0;

      const info = await invoke('video:open', { path: paths[0] });
      videoInfo = info;
      videoUrl = getBaseUrl() + info.url + '?t=' + Date.now();
      duration = info.duration;

      // Fetch analysis data in parallel
      const [wf, br] = await Promise.all([
        invoke('video:waveform', { samples: 2000 }),
        invoke('video:bitrate', { step: 0.5 })
      ]);
      waveformData = wf;
      bitrateData = br;
    } catch (e) {
      errorMsg = e.message || 'Failed to open video';
      console.error('[App] Error:', e);
    } finally {
      loading = false;
    }
  }

  function handleSeek(time) {
    seekTarget = { time: Math.max(0, Math.min(time, duration)), id: Date.now() };
  }

  function fmtTime(s) {
    if (!s || isNaN(s)) return '0:00';
    const m = Math.floor(s / 60);
    const sec = Math.floor(s % 60);
    return `${m}:${sec.toString().padStart(2, '0')}`;
  }

  function fmtBytes(b) {
    if (b >= 1e9) return (b / 1e9).toFixed(1) + ' GB';
    if (b >= 1e6) return (b / 1e6).toFixed(1) + ' MB';
    if (b >= 1e3) return (b / 1e3).toFixed(1) + ' KB';
    return b + ' B';
  }
</script>

<main class="h-screen flex flex-col overflow-hidden" style="background: var(--bg); color: var(--text);">
  <!-- Header -->
  <header class="flex items-center gap-4 px-6 py-3 shrink-0 relative" style="border-bottom: none;">
    <!-- Gradient bottom border -->
    <div class="absolute bottom-0 left-0 right-0 h-[2px]" style="background: linear-gradient(90deg, #0072F0, #00E1C9); opacity: 0.6;"></div>

    <h1 class="text-base font-semibold tracking-tight whitespace-nowrap">
      <span style="background: linear-gradient(135deg, #0072F0, #00E1C9); -webkit-background-clip: text; -webkit-text-fill-color: transparent;">LibAnyar</span> Video Player
    </h1>

    <button
      onclick={openFile}
      disabled={loading}
      class="px-4 py-1.5 text-white text-sm font-medium rounded-lg transition-all disabled:opacity-50 cursor-pointer"
      style="background: linear-gradient(135deg, #0072F0, #00E1C9);"
      onmouseenter={(e) => e.target.style.boxShadow = '0 2px 12px rgba(0,225,201,0.15)'}
      onmouseleave={(e) => e.target.style.boxShadow = 'none'}
    >
      {loading ? 'Loading…' : 'Open File'}
    </button>

    {#if videoInfo}
      <div class="flex items-center gap-3 text-xs ml-auto" style="color: var(--text-dim);">
        <span>{videoInfo.width}×{videoInfo.height}</span>
        <span style="color: var(--border);">|</span>
        <span>{videoInfo.videoCodec}{videoInfo.audioCodec ? ' / ' + videoInfo.audioCodec : ''}</span>
        <span style="color: var(--border);">|</span>
        <span>{Number(videoInfo.fps).toFixed(1)} fps</span>
        <span style="color: var(--border);">|</span>
        <span>{fmtTime(videoInfo.duration)}</span>
        <span style="color: var(--border);">|</span>
        <span>{fmtBytes(videoInfo.fileSize)}</span>
      </div>
    {/if}
  </header>

  <!-- Error -->
  {#if errorMsg}
    <div class="px-6 py-2 text-sm" style="background: rgba(233,44,55,0.1); color: var(--danger);">
      {errorMsg}
    </div>
  {/if}

  <!-- Content -->
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div
    bind:this={contentEl}
    class="flex-1 flex flex-col min-h-0 relative"
    onmousemove={handleContentMouseMove}
    onmouseleave={handleContentMouseLeave}
  >
    {#if videoUrl}
      <!-- Video: fills the entire content area -->
      <div class="flex-1 min-h-0">
        <VideoPlayer
          audioSrc={videoUrl}
          {videoInfo}
          bind:currentTime
          bind:duration
          bind:playing
          {seekTarget}
          ontoggleplay={(fn) => vpTogglePlay = fn}
        />
      </div>

      <!-- Transport controls: always visible at bottom, highest z-index -->
      <div
        class="absolute bottom-0 left-0 right-0 flex items-center gap-3 px-4 py-2 z-30"
        style="background: linear-gradient(transparent, rgba(0,0,0,0.7));"
      >
        <button
          onclick={() => vpTogglePlay && vpTogglePlay()}
          class="text-white text-lg font-medium cursor-pointer w-8 h-8 flex items-center justify-center rounded"
          style="background: rgba(255,255,255,0.15);"
        >
          {playing ? '⏸' : '▶'}
        </button>

        <span class="text-white text-xs opacity-70">
          {fmtTime(currentTime)} / {fmtTime(duration)}
        </span>

        <span class="text-xs ml-auto" style="color: rgba(255,255,255,0.3);">
          {videoInfo?.width}×{videoInfo?.height} SharedBuffer WebGL{gpuRenderer ? ` · ${gpuRenderer}` : ''}
        </span>
      </div>

      <!-- Bottom panel: absolutely positioned overlay, slides up/down -->
      <!-- svelte-ignore a11y_no_static_element_interactions -->
      <div
        bind:this={bottomPanelEl}
        class="absolute left-0 right-0 flex flex-col gap-1 px-2 pb-1 z-20"
        style="bottom: 2.5rem; background: linear-gradient(transparent, var(--bg) 24px); transition: transform 0.35s cubic-bezier(0.4, 0, 0.2, 1), opacity 0.35s ease; transform: translateY({panelVisible ? '0' : '100%'}); opacity: {panelVisible ? '1' : '0'}; pointer-events: {panelVisible ? 'auto' : 'none'};"
        onmouseenter={showPanel}
      >
        {#if waveformData?.peaks?.length}
          <section>
            <div class="flex items-center gap-2 mb-0.5">
              <span class="text-xs font-medium" style="color: var(--text-dim);">Audio Waveform</span>
              <span class="text-xs" style="color: var(--chart-audio);">{fmtTime(currentTime)} / {fmtTime(duration)}</span>
            </div>
            <Waveform
              peaks={waveformData.peaks}
              {duration}
              {currentTime}
              onseek={handleSeek}
              padLeft={showBitrate && bitrateData ? chartPlotLeft : 0}
            />
          </section>
        {/if}

        <!-- Bitrate toggle -->
        <div class="flex items-center gap-2">
          <button
            onclick={() => showBitrate = !showBitrate}
            class="flex items-center gap-2 px-3 py-0.5 text-xs font-medium rounded-md border transition-colors cursor-pointer"
            style="border-color: {showBitrate ? 'var(--accent)' : 'var(--border)'}; color: {showBitrate ? 'var(--accent)' : 'var(--text-dim)'}; background: transparent;"
          >
            <span
              class="w-2 h-2 rounded-full transition-colors"
              style="background: {showBitrate ? 'var(--accent)' : 'var(--border)'};"
            ></span>
            Bitrate Monitor
          </button>

          {#if showBitrate && bitrateData}
            <div class="flex items-center gap-3 text-xs" style="color: var(--text-dim);">
              <span class="flex items-center gap-1">
                <span class="inline-block w-2.5 h-0.5 rounded" style="background: var(--chart-video);"></span>
                Video
              </span>
              <span class="flex items-center gap-1">
                <span class="inline-block w-2.5 h-0.5 rounded" style="background: var(--chart-audio);"></span>
                Audio
              </span>
            </div>
          {/if}
        </div>

        {#if showBitrate && bitrateData}
          <BitrateChart
            timestamps={bitrateData.timestamps}
            videoBps={bitrateData.videoBps}
            audioBps={bitrateData.audioBps}
            {currentTime}
            {duration}
            onseek={handleSeek}
            onlayout={(l, w) => { chartPlotLeft = l; chartPlotWidth = w; }}
          />
        {/if}
      </div>
    {:else if !loading}
      <!-- Empty state -->
      <div class="flex-1 flex flex-col items-center justify-center gap-4" style="color: var(--text-dim);">
        <svg class="w-16 h-16 opacity-25" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5" d="M15 10l4.553-2.276A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M5 18h8a2 2 0 002-2V8a2 2 0 00-2-2H5a2 2 0 00-2 2v8a2 2 0 002 2z"/>
        </svg>
        <p class="text-sm">Open a video file to begin</p>
        <p class="text-xs opacity-50">Supports MP4, WebM, MKV, AVI, MOV, OGG</p>
      </div>
    {/if}
  </div>
</main>
