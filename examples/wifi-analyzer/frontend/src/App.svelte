<script>
  import { invoke, listen, onReady } from '@libanyar/api';
  import { buffer, canvas } from '@libanyar/api';

  const { fetchBuffer } = buffer;
  const { createFrameRenderer } = canvas;

  /** @type {HTMLCanvasElement | undefined} */
  let canvasEl = $state(undefined);

  /** @type {ReturnType<typeof createFrameRenderer> | null} */
  let renderer = $state(null);

  /** @type {Array<{ssid: string, bssid: string, channel: number, frequency: number, signal: number, bandwidth: number, security: string, associated: boolean}>} */
  let networks = $state([]);

  let scanning = $state(true);
  let iface = $state('');
  let frameCount = $state(0);
  let error = $state('');
  let gpuRenderer = $state('');
  let activeScanBusy = $state(false);
  let unlistenActiveScan = $state(null);

  // Sort mode for the network table
  let sortBy = $state('signal'); // 'signal' | 'channel' | 'ssid'

  // ── Tooltip on peak hover ─────────────────────────────────────────────
  // C++ palette (same order as render_spectrum)
  const PALETTE = [
    '#42a5f5','#ef5350','#66bb6a','#ffa726',
    '#ab47bc','#26c6da','#fff176','#8d6e63',
    '#ec407a','#00e676',
  ];
  const CH5 = [36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,
               132,136,140,144,149,153,157,161,165];

  /** Mirror of C++ ch_to_px — returns center x in 800×400 canvas coords */
  function peakCx(ch) {
    if (ch >= 1 && ch <= 14) return 50 + ((ch - 1) / 13) * 277;
    const i = CH5.indexOf(ch);
    return i >= 0 ? 347 + (i / 24) * 433 : null;
  }

  /** Mirror of C++ peak_y in 800×400 canvas coords */
  function peakCy(signal) {
    const norm = Math.max(0, Math.min(1, (signal + 100) / 80));
    return 30 + 330 - norm * 330;
  }

  /** @type {HTMLDivElement | undefined} */
  let canvasWrap = $state(undefined);

  /** @type {{ ap: object, color: string, px: number, py: number } | null} */
  let tooltip = $state(null);

  const HIT_RADIUS = 18; // canvas units — peak dot is ±2 px

  function onCanvasMouseMove(e) {
    if (!canvasWrap || networks.length === 0) { tooltip = null; return; }
    const rect = canvasWrap.getBoundingClientRect();
    // Convert CSS mouse coords → 800×400 canvas space
    const mx = (e.clientX - rect.left) / rect.width  * 800;
    const my = (e.clientY - rect.top)  / rect.height * 400;

    let best = null, bestDist = HIT_RADIUS;
    networks.forEach((ap, i) => {
      const cx = peakCx(ap.channel);
      if (cx === null) return;
      const cy = peakCy(ap.signal);
      const d = Math.hypot(mx - cx, my - cy);
      if (d < bestDist) {
        bestDist = d;
        // Convert peak back to CSS px for tooltip position
        const cssPx = (cx / 800) * rect.width;
        const cssPy = (cy / 400) * rect.height;
        // Flip tooltip below peak when peak is in top 38% of the canvas
        const flip = cssPy < rect.height * 0.38;
        best = { ap, color: PALETTE[i % PALETTE.length], px: cssPx, py: cssPy, flip };
      }
    });
    tooltip = best;
  }

  function onCanvasMouseLeave() { tooltip = null; }

  // Detect GPU renderer name
  function detectGpu() {
    try {
      const c = document.createElement('canvas');
      const gl = c.getContext('webgl');
      if (gl) {
        const ext = gl.getExtension('WEBGL_debug_renderer_info');
        if (ext) {
          gpuRenderer = gl.getParameter(ext.UNMASKED_RENDERER_WEBGL);
        }
      }
    } catch {}
  }

  /** Sorted networks */
  let sortedNetworks = $derived.by(() => {
    const copy = [...networks];
    if (sortBy === 'signal') {
      copy.sort((a, b) => b.signal - a.signal);
    } else if (sortBy === 'channel') {
      copy.sort((a, b) => a.channel - b.channel);
    } else {
      copy.sort((a, b) => a.ssid.localeCompare(b.ssid));
    }
    return copy;
  });

  /** Signal strength → CSS color */
  function signalColor(dbm) {
    if (dbm >= -50) return 'var(--success)';
    if (dbm >= -70) return 'var(--warning)';
    return 'var(--danger)';
  }

  /** Signal bar width (0–100%) */
  function signalWidth(dbm) {
    // Map -100..-20 → 0..100%
    return Math.max(0, Math.min(100, ((dbm + 100) / 80) * 100));
  }

  /** Security display label */
  function secLabel(sec) {
    if (!sec || sec === 'Open') return 'Open';
    if (sec.includes('WPA3')) return 'WPA3';
    if (sec.includes('WPA2')) return 'WPA2';
    if (sec.includes('WPA')) return 'WPA';
    if (sec.includes('WEP')) return 'WEP';
    return sec;
  }

  /** Security badge class */
  function secClass(sec) {
    if (!sec || sec === 'Open') return 'badge-danger';
    return 'badge-muted';
  }

  async function startScanning() {
    error = '';
    try {
      const res = await invoke('wifi:start');
      scanning = true;
      iface = res.interface || '';
    } catch (e) {
      error = `Start failed: ${e.message || e}`;
    }
  }

  async function activeScan() {
    error = '';
    activeScanBusy = true;
    try {
      await invoke('wifi:active-scan');
    } catch (e) {
      error = `Active scan failed: ${e.message || e}`;
      activeScanBusy = false;
    }
  }

  // Listen for buffer:ready and render manually (no pool-release needed;
  // the plugin manages its own double-buffered SharedBufferPool internally).
  $effect(() => {
    if (!canvasEl) return;

    /** @type {ReturnType<typeof createFrameRenderer> | null} */
    let r = null;

    const unlisten = listen('buffer:ready', async (event) => {
      if (event.pool !== 'wifi-spectrum') return;

      try {
        const data = await fetchBuffer(event.url);
        const meta = event.metadata;

        // Create renderer on first frame
        if (!r) {
          const w = (meta && typeof meta.width === 'number') ? meta.width : 800;
          const h = (meta && typeof meta.height === 'number') ? meta.height : 400;
          r = createFrameRenderer({
            canvas: canvasEl,
            width: w,
            height: h,
            format: 'rgba',
          });
          renderer = r;
        }

        r.drawFrame(data);
        frameCount++;

        // Extract network list from metadata
        if (meta && Array.isArray(meta.networks)) {
          networks = meta.networks;
        }
      } catch (err) {
        console.error('[WiFiAnalyzer] Frame error:', err);
      }
    });

    return () => {
      unlisten();
      if (r) { r.destroy(); r = null; }
      renderer = null;
    };
  });

  // Auto-start on ready
  onReady(() => {
    detectGpu();
    startScanning();

    // Listen for active-scan completion event
    unlistenActiveScan = listen('wifi:active-scan-done', (event) => {
      activeScanBusy = false;
      if (event.ok === false) {
        error = event.error || 'Active scan failed';
      }
    });
  });
</script>

<div class="flex flex-col h-screen">
  <!-- ── Header ── -->
  <header class="flex items-center justify-between px-5 py-2.5"
          style="background: var(--surface); border-bottom: 1px solid var(--border);">
    <div class="flex items-center gap-3">
      <div class="flex items-center gap-2">
        <svg class="w-[18px] h-[18px]" style="color: var(--accent);" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                d="M8.111 16.404a5.5 5.5 0 017.778 0M12 20h.01m-7.08-7.071c3.904-3.905 10.236-3.905 14.141 0M1.394 9.393c5.857-5.858 15.355-5.858 21.213 0" />
        </svg>
        <span class="text-sm font-semibold" style="color: var(--text);">WiFi Analyzer</span>
      </div>
      {#if iface}
        <span class="badge badge-muted">{iface}</span>
      {/if}
      {#if scanning}
        <span class="scan-dot"></span>
      {/if}
    </div>
    <div class="flex items-center gap-2">
      <button onclick={activeScan}
              disabled={activeScanBusy}
              class="btn btn-accent">
        <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                d="M13 10V3L4 14h7v7l9-11h-7z" />
        </svg>
        {activeScanBusy ? 'Scanning…' : 'Active Scan'}
      </button>
    </div>
  </header>

  <!-- ── Main content ── -->
  <div class="flex-1 flex flex-col overflow-hidden min-h-0">
    <!-- Spectrum Canvas -->
    <div bind:this={canvasWrap}
         class="flex-1 relative min-h-0"
         style="background: var(--surface);"
         onmousemove={onCanvasMouseMove}
         onmouseleave={onCanvasMouseLeave}>
      <canvas bind:this={canvasEl}
              width="800" height="400"
              class="w-full h-full">
      </canvas>
      <!-- SVG axis labels — crisp vector text, scales with canvas -->
      <!-- viewBox matches canvas intrinsic size (800×400); overflow:visible lets labels in margins show -->
      <svg viewBox="0 0 800 400"
           preserveAspectRatio="none"
           class="absolute inset-0 w-full h-full pointer-events-none"
           xmlns="http://www.w3.org/2000/svg"
           style="font-family: 'Inter', system-ui, sans-serif; overflow: visible;">
        <!-- dBm unit -->
        <text x="16" y="20" text-anchor="start" font-size="10" fill="#6e767a">dBm</text>

        <!-- Y-axis dBm value labels  (margin_left=50, plot_h=330, margin_top=30) -->
        <!-- y = margin_top + plot_h − (dbm+100)/80 × plot_h -->
        <text x="44" y="30"    text-anchor="end" dominant-baseline="middle" font-size="10" fill="#788387">-20</text>
        <text x="44" y="112.5" text-anchor="end" dominant-baseline="middle" font-size="10" fill="#788387">-40</text>
        <text x="44" y="195"   text-anchor="end" dominant-baseline="middle" font-size="10" fill="#788387">-60</text>
        <text x="44" y="277.5" text-anchor="end" dominant-baseline="middle" font-size="10" fill="#788387">-80</text>
        <text x="44" y="360"   text-anchor="end" dominant-baseline="middle" font-size="10" fill="#788387">-100</text>

        <!-- 2.4 GHz channel labels  x = 50 + (ch-1)/13 × 277 -->
        <text x="50"    y="374" text-anchor="middle" font-size="9" fill="#788387">1</text>
        <text x="71.3"  y="374" text-anchor="middle" font-size="9" fill="#788387">2</text>
        <text x="92.6"  y="374" text-anchor="middle" font-size="9" fill="#788387">3</text>
        <text x="113.8" y="374" text-anchor="middle" font-size="9" fill="#788387">4</text>
        <text x="135.1" y="374" text-anchor="middle" font-size="9" fill="#788387">5</text>
        <text x="156.4" y="374" text-anchor="middle" font-size="9" fill="#788387">6</text>
        <text x="177.7" y="374" text-anchor="middle" font-size="9" fill="#788387">7</text>
        <text x="198.9" y="374" text-anchor="middle" font-size="9" fill="#788387">8</text>
        <text x="220.2" y="374" text-anchor="middle" font-size="9" fill="#788387">9</text>
        <text x="241.5" y="374" text-anchor="middle" font-size="9" fill="#788387">10</text>
        <text x="262.8" y="374" text-anchor="middle" font-size="9" fill="#788387">11</text>
        <text x="284.1" y="374" text-anchor="middle" font-size="9" fill="#788387">12</text>
        <text x="305.4" y="374" text-anchor="middle" font-size="9" fill="#788387">13</text>

        <!-- 2.4 GHz band label -->
        <text x="178" y="391" text-anchor="middle" font-size="9" fill="#6e767a">2.4 GHz</text>

        <!-- 5 GHz channel labels  x = 347 + i/24 × 433  (every 4th of 25 channels) -->
        <text x="347" y="374" text-anchor="middle" font-size="9" fill="#788387">36</text>
        <text x="419" y="374" text-anchor="middle" font-size="9" fill="#788387">52</text>
        <text x="491" y="374" text-anchor="middle" font-size="9" fill="#788387">100</text>
        <text x="564" y="374" text-anchor="middle" font-size="9" fill="#788387">116</text>
        <text x="636" y="374" text-anchor="middle" font-size="9" fill="#788387">132</text>
        <text x="708" y="374" text-anchor="middle" font-size="9" fill="#788387">149</text>
        <text x="780" y="374" text-anchor="middle" font-size="9" fill="#788387">165</text>

        <!-- 5 GHz band label -->
        <text x="564" y="391" text-anchor="middle" font-size="9" fill="#6e767a">5 GHz</text>
      </svg>

      <!-- Peak tooltip -->
      {#if tooltip}
        {@const ap = tooltip.ap}
        <div class="peak-tooltip"
             class:flip={tooltip.flip}
             style="left: {tooltip.px}px; top: {tooltip.py}px; --arrow-left: {tooltip.arrowLeft}px;">
          <div class="peak-tooltip-header">
            <span class="peak-dot" style="background: {tooltip.color};"></span>
            <span class="peak-ssid">{ap.ssid || '(Hidden)'}</span>
            {#if ap.associated}
              <span class="badge badge-accent" style="font-size:10px;padding:1px 5px;">Connected</span>
            {/if}
          </div>
          <div class="peak-tooltip-rows">
            <div class="peak-row"><span>Channel</span><span>{ap.channel}</span></div>
            <div class="peak-row"><span>Freq</span><span>{ap.frequency} MHz</span></div>
            <div class="peak-row"><span>Signal</span>
              <span style="color: {ap.signal >= -50 ? 'var(--success)' : ap.signal >= -70 ? 'var(--warning)' : 'var(--danger)'}; font-weight:600;">{ap.signal} dBm</span>
            </div>
            <div class="peak-row"><span>Width</span><span>{ap.bandwidth} MHz</span></div>
            <div class="peak-row"><span>Security</span><span>{ap.security || 'Open'}</span></div>
          </div>
        </div>
      {/if}
    </div>

    <div class="separator"></div>

    <!-- ── Network table ── -->
    <div class="shrink-0 overflow-auto" style="background: var(--bg); max-height: 40vh;">
      <!-- Table header -->
      <div class="sticky top-0 z-10 grid grid-cols-[2.5fr_0.7fr_0.8fr_0.7fr_0.7fr_3fr] gap-2 px-5 py-2 text-[11px] font-semibold uppercase tracking-wide"
           style="background: var(--surface); color: var(--text-muted); border-bottom: 1px solid var(--border);">
        <button class="text-left cursor-pointer transition-colors flex items-center gap-1"
                style="color: {sortBy === 'ssid' ? 'var(--text-dim)' : 'var(--text-muted)'}; background: none; border: none; font: inherit; font-size: inherit; font-weight: inherit; text-transform: inherit; letter-spacing: inherit;"
                onclick={() => sortBy = 'ssid'}>
          SSID
          {#if sortBy === 'ssid'}
            <svg class="w-2.5 h-2.5" fill="currentColor" viewBox="0 0 20 20"><path d="M5 8l5-5 5 5H5z"/></svg>
          {/if}
        </button>
        <button class="text-left cursor-pointer transition-colors flex items-center gap-1"
                style="color: {sortBy === 'channel' ? 'var(--text-dim)' : 'var(--text-muted)'}; background: none; border: none; font: inherit; font-size: inherit; font-weight: inherit; text-transform: inherit; letter-spacing: inherit;"
                onclick={() => sortBy = 'channel'}>
          Ch
          {#if sortBy === 'channel'}
            <svg class="w-2.5 h-2.5" fill="currentColor" viewBox="0 0 20 20"><path d="M5 8l5-5 5 5H5z"/></svg>
          {/if}
        </button>
        <span>Freq</span>
        <span>BW</span>
        <span>Sec</span>
        <button class="text-left cursor-pointer transition-colors flex items-center gap-1"
                style="color: {sortBy === 'signal' ? 'var(--text-dim)' : 'var(--text-muted)'}; background: none; border: none; font: inherit; font-size: inherit; font-weight: inherit; text-transform: inherit; letter-spacing: inherit;"
                onclick={() => sortBy = 'signal'}>
          Signal
          {#if sortBy === 'signal'}
            <svg class="w-2.5 h-2.5" fill="currentColor" viewBox="0 0 20 20"><path d="M15 12l-5 5-5-5h10z"/></svg>
          {/if}
        </button>
      </div>

      <!-- Network rows -->
      {#each sortedNetworks as ap}
        <div class="net-row grid grid-cols-[2.5fr_0.7fr_0.8fr_0.7fr_0.7fr_3fr] gap-2 px-5 py-2.5 text-[13px] items-center"
             class:connected={ap.associated}
             style="border-bottom: 1px solid var(--border);">
          <div class="truncate font-medium flex items-center gap-2"
               style="color: {ap.associated ? 'var(--accent)' : 'var(--text)'};">
            <span class="truncate">{ap.ssid || '(Hidden)'}</span>
            {#if ap.associated}
              <span class="badge badge-accent">Connected</span>
            {/if}
          </div>
          <span class="tabular-nums" style="color: var(--text-dim);">{ap.channel}</span>
          <span class="tabular-nums" style="color: var(--text-muted);">{ap.frequency}</span>
          <span class="badge badge-muted">{ap.bandwidth}M</span>
          <span class="badge {secClass(ap.security)}">
            {secLabel(ap.security)}
          </span>
          <div class="flex items-center gap-2.5">
            <div class="flex-1 h-[5px] rounded-full overflow-hidden" style="background: var(--surface-3);">
              <div class="h-full rounded-full transition-all duration-300"
                   style="width: {signalWidth(ap.signal)}%; background: {signalColor(ap.signal)};">
              </div>
            </div>
            <span class="text-xs tabular-nums w-14 text-right font-medium" style="color: {signalColor(ap.signal)};">
              {ap.signal} dBm
            </span>
          </div>
        </div>
      {:else}
        <div class="flex flex-col items-center justify-center py-12 gap-2.5" style="color: var(--text-muted);">
          <svg class="w-8 h-8 opacity-25" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5"
                  d="M8.111 16.404a5.5 5.5 0 017.778 0M12 20h.01m-7.08-7.071c3.904-3.905 10.236-3.905 14.141 0M1.394 9.393c5.857-5.858 15.355-5.858 21.213 0" />
          </svg>
          <span class="text-[13px]">{scanning ? 'Scanning for networks…' : 'Press Active Scan to begin'}</span>
        </div>
      {/each}
    </div>
  </div>

  <!-- ── Status bar ── -->
  <footer class="flex items-center justify-between px-5 py-2 text-xs"
          style="border-top: 1px solid var(--border); background: var(--surface); color: var(--text-muted);">
    <div class="flex items-center gap-2.5">
      <span class="badge badge-muted">{networks.length} networks</span>
      <span style="color: var(--border-light);">·</span>
      <span class="tabular-nums">{frameCount} frames</span>
    </div>
    <div class="flex items-center gap-2.5">
      <span class="kbd">800×400</span>
      <span style="color: var(--border-light);">·</span>
      <span>SharedBuffer WebGL</span>
      {#if gpuRenderer}
        <span style="color: var(--border-light);">·</span>
        <span class="kbd">{gpuRenderer}</span>
      {/if}
      <span style="width: 1px; height: 12px; background: var(--border-light);"></span>
      <img src="/assets/text.png" alt="LibAnyar" class="h-[13px]" style="opacity: 0.5;" />
    </div>
  </footer>

  <!-- ── Error toast ── -->
  {#if error}
    <div class="toast">
      <svg class="w-4 h-4 shrink-0" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
              d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-2.5L13.732 4.5c-.77-.833-2.694-.833-3.464 0L3.34 16.5c-.77.833.192 2.5 1.732 2.5z" />
      </svg>
      <span class="flex-1">{error}</span>
      <button class="btn-ghost" style="padding: 2px 4px; border-radius: 4px;" onclick={() => error = ''} title="Dismiss">
        <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12" />
        </svg>
      </button>
    </div>
  {/if}
</div>
