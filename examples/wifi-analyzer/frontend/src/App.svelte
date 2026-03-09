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
  <!-- Header -->
  <header class="flex items-center justify-between px-4 py-2 relative"
          style="background: var(--surface);">
    <!-- gradient bottom border -->
    <div class="absolute bottom-0 left-0 right-0" style="height: 2px; background: var(--gradient-h); opacity: 0.6;"></div>
    <div class="flex items-center gap-3">
      <div class="text-base font-semibold">
        <span>📡 </span><span style="background: var(--gradient); -webkit-background-clip: text; -webkit-text-fill-color: transparent;">WiFi Channel Analyzer</span>
      </div>
      {#if iface}
        <span class="text-xs px-2 py-0.5 rounded"
              style="background: var(--surface-2); color: var(--text-dim);">
          {iface}
        </span>
      {/if}
    </div>
    <div class="flex items-center gap-2">
      <button onclick={activeScan}
              disabled={activeScanBusy}
              class="px-3 py-1 text-xs font-semibold rounded cursor-pointer disabled:opacity-50 transition-shadow"
              style="background: var(--gradient); color: #0A0A0A; border: none;"
              onmouseenter={(e) => e.currentTarget.style.boxShadow = '0 2px 14px var(--accent-glow)'}
              onmouseleave={(e) => e.currentTarget.style.boxShadow = 'none'}>
        {activeScanBusy ? 'Scanning...' : '⚡ Active Scan'}
      </button>
    </div>
  </header>

  <!-- Main content -->
  <div class="flex-1 flex flex-col overflow-hidden min-h-0">
    <!-- Spectrum Canvas — fills all remaining vertical space -->
    <div class="flex-1 relative min-h-0" style="background: var(--surface);">
      <canvas bind:this={canvasEl}
              width="800" height="400"
              class="w-full h-full">
      </canvas>
      <!-- dBm unit label -->
      <div class="absolute top-1 left-1 text-[9px]" style="color: var(--text-dim);">
        dBm
      </div>
    </div>

    <!-- Network list — natural height, scrolls if many rows -->
    <div class="shrink-0 overflow-auto" style="background: var(--bg); max-height: 40vh;">
      <!-- Table header -->
      <div class="sticky top-0 grid grid-cols-[2fr_1fr_1fr_1fr_1fr_3fr] gap-1 px-3 py-1.5 text-[10px] font-semibold uppercase"
           style="background: var(--surface); color: var(--text-dim); border-bottom: 1px solid var(--border);">
        <button class="text-left cursor-pointer hover:underline" onclick={() => sortBy = 'ssid'}>
          SSID {sortBy === 'ssid' ? '▲' : ''}
        </button>
        <button class="text-left cursor-pointer hover:underline" onclick={() => sortBy = 'channel'}>
          Ch {sortBy === 'channel' ? '▲' : ''}
        </button>
        <span>Freq</span>
        <span>BW</span>
        <span>Sec</span>
        <button class="text-left cursor-pointer hover:underline" onclick={() => sortBy = 'signal'}>
          Signal {sortBy === 'signal' ? '▼' : ''}
        </button>
      </div>

      <!-- Network rows -->
      {#each sortedNetworks as ap}
        <div class="grid grid-cols-[2fr_1fr_1fr_1fr_1fr_3fr] gap-1 px-3 py-1.5 text-xs items-center hover:opacity-80"
             style="border-bottom: 1px solid var(--border); {ap.associated ? 'background: var(--accent-dim);' : ''}">
          <div class="truncate font-medium" style="color: {ap.associated ? 'var(--accent)' : 'var(--text)'};">
            {ap.ssid}
            {#if ap.associated}
              <span class="ml-1 text-[9px]" style="color: var(--accent);">●</span>
            {/if}
          </div>
          <span style="color: var(--text-dim);">{ap.channel}</span>
          <span style="color: var(--text-dim);">{ap.frequency}</span>
          <span style="color: var(--text-dim);">{ap.bandwidth} MHz</span>
          <span class="text-[10px]" style="color: {ap.security === 'Open' ? 'var(--danger)' : 'var(--text-dim)'};">
            {ap.security}
          </span>
          <div class="flex items-center gap-2">
            <div class="flex-1 h-2 rounded-full overflow-hidden" style="background: var(--surface-2);">
              <div class="h-full rounded-full transition-all"
                   style="width: {signalWidth(ap.signal)}%; background: {signalColor(ap.signal)};">
              </div>
            </div>
            <span class="text-[10px] tabular-nums w-10 text-right" style="color: {signalColor(ap.signal)};">
              {ap.signal} dBm
            </span>
          </div>
        </div>
      {:else}
        <div class="text-center py-8 text-sm" style="color: var(--text-dim);">
          {scanning ? 'Scanning for networks...' : 'Press "Start Scan" to begin'}
        </div>
      {/each}
    </div>
  </div>

  <!-- Status bar -->
  <footer class="flex items-center justify-between px-3 py-1 text-[10px] border-t"
          style="border-color: var(--border); background: var(--surface); color: var(--text-dim);">
    <span>
      {networks.length} networks ·
      {frameCount} frames
    </span>
    <span>
      800×400 SharedBuffer WebGL{gpuRenderer ? ` · ${gpuRenderer}` : ''}
    </span>
  </footer>

  {#if error}
    <div class="fixed bottom-10 left-1/2 -translate-x-1/2 px-4 py-2 rounded text-xs"
         style="background: var(--danger); color: white;">
      {error}
    </div>
  {/if}
</div>
