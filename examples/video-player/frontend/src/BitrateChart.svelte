<script>
  /**
   * BitrateChart — Time-series bitrate chart using uPlot.
   * Shows video (purple) and audio (cyan) bitrate.
   * Time cursor syncs with video currentTime. Click to seek.
   *
   * Reactivity notes:
   * - `chart` is a plain `let` (NOT $state) so writing it from the
   *   creation $effect never triggers reactive cascades.
   * - `plotLeft`/`plotWidth` are plain `let` for the same reason.
   * - A `$state` counter `geoSeq` is bumped after geometry is computed
   *   so the cursor $effect re-evaluates when the chart is ready.
   * - `onlayout` (which writes parent $state) is always called OUTSIDE
   *   any $effect scope (via queueMicrotask or ResizeObserver callback)
   *   to prevent effect_update_depth_exceeded.
   */
  import uPlot from 'uplot';
  import 'uplot/dist/uPlot.min.css';
  import { onDestroy } from 'svelte';

  let {
    timestamps = [],
    videoBps = [],
    audioBps = [],
    currentTime = 0,
    duration = 0,
    onseek,
    onlayout,
  } = $props();

  let wrapEl = $state(null);
  let chartEl = $state(null);
  let cursorEl = $state(null);

  // NOT reactive — avoids all effect coupling with chart creation
  let chart = null;
  let plotLeft = 0;
  let plotWidth = 0;
  let resizeObs = null;

  // Bumped after geometry is (re-)computed so the cursor $effect re-runs
  let geoSeq = $state(0);

  const CHART_HEIGHT = 180;

  function fmtAxisTime(v) {
    const m = Math.floor(v / 60);
    const s = Math.floor(v % 60);
    return `${m}:${s.toString().padStart(2, '0')}`;
  }

  function fmtBps(v) {
    if (v >= 1e6) return (v / 1e6).toFixed(1) + 'M';
    if (v >= 1e3) return (v / 1e3).toFixed(0) + 'K';
    return v.toFixed(0);
  }

  // Read plot-area geometry from uPlot bbox and notify parent.
  // Called ONLY from ResizeObserver (outside any $effect).
  function refreshGeometry() {
    if (!chart) return;
    const bbox = chart.bbox;
    plotLeft = bbox.left / devicePixelRatio;
    plotWidth = bbox.width / devicePixelRatio;
    geoSeq++;                       // nudge cursor $effect
    onlayout?.(plotLeft, plotWidth); // safe — not inside an $effect
  }

  // ── Create / recreate chart when data props change ─────────────────
  $effect(() => {
    if (!chartEl || timestamps.length === 0) return;

    // Cleanup previous chart (chart is plain let → no reactive read)
    if (chart) chart.destroy();
    if (resizeObs) { resizeObs.disconnect(); resizeObs = null; }

    const data = [
      new Float64Array(timestamps),
      new Float64Array(videoBps),
      new Float64Array(audioBps),
    ];

    const opts = {
      width: chartEl.clientWidth || 800,
      height: CHART_HEIGHT,
      padding: [12, 16, 0, 0],
      cursor: {
        show: true,
        x: true,
        y: false,
        drag: { x: false, y: false },
        points: { show: false },
      },
      legend: { show: false },
      scales: {
        x: { time: false },
        y: { auto: true, range: (u, min, max) => [0, max * 1.1] },
      },
      axes: [
        {
          stroke: '#52525b',
          grid: { show: true, stroke: 'rgba(82, 82, 91, 0.2)', width: 1 },
          ticks: { show: true, stroke: '#3f3f46', size: 4 },
          font: '10px system-ui',
          values: (u, vals) => vals.map(fmtAxisTime),
          gap: 8,
        },
        {
          stroke: '#52525b',
          grid: { show: true, stroke: 'rgba(82, 82, 91, 0.2)', width: 1 },
          ticks: { show: true, stroke: '#3f3f46', size: 4 },
          font: '10px system-ui',
          values: (u, vals) => vals.map(fmtBps),
          size: 50,
          gap: 4,
        },
      ],
      series: [
        {},
        {
          label: 'Video',
          stroke: '#0072F0',
          fill: 'rgba(0, 114, 240, 0.12)',
          width: 1.5,
          paths: uPlot.paths.bars({ size: [0.6, 100] }),
        },
        {
          label: 'Audio',
          stroke: '#E2B632',
          fill: 'rgba(226, 182, 50, 0.12)',
          width: 1.5,
          paths: uPlot.paths.bars({ size: [0.3, 100] }),
        },
      ],
    };

    const c = new uPlot(opts, data, chartEl);
    chart = c;

    // Read geometry synchronously so cursor works on the very first frame
    const bbox = c.bbox;
    plotLeft = bbox.left / devicePixelRatio;
    plotWidth = bbox.width / devicePixelRatio;

    // Notify parent via microtask (outside this $effect's tracking scope)
    queueMicrotask(() => {
      onlayout?.(plotLeft, plotWidth);
      geoSeq++;   // trigger cursor $effect
    });

    // ResizeObserver handles subsequent geometry updates
    resizeObs = new ResizeObserver(() => {
      if (chart && chartEl) {
        chart.setSize({ width: chartEl.clientWidth, height: CHART_HEIGHT });
        refreshGeometry();
      }
    });
    resizeObs.observe(chartEl);
  });

  // ── Position cursor overlay ────────────────────────────────────────
  // Uses simple linear interpolation instead of chart.valToPos() so
  // there is zero dependency on the chart object (plain let).
  // Tracked deps: currentTime, cursorEl, geoSeq, duration.
  $effect(() => {
    const t = currentTime;
    const el = cursorEl;
    const _ = geoSeq;          // re-run when chart geometry changes

    if (!el || plotWidth <= 0 || duration <= 0) {
      if (el) el.style.opacity = '0';
      return;
    }

    const frac = Math.max(0, Math.min(t / duration, 1));
    const x = plotLeft + frac * plotWidth;
    el.style.transform = `translateX(${x}px)`;
    el.style.opacity = '1';
  });

  // Click to seek (uses chart.posToVal for accuracy)
  function handleClick(e) {
    if (!chart || !wrapEl || duration <= 0 || !onseek) return;
    try {
      const rect = chartEl.getBoundingClientRect();
      const x = e.clientX - rect.left;
      const time = chart.posToVal(x, 'x');
      if (time >= 0 && time <= duration) onseek(time);
    } catch (_) {}
  }

  onDestroy(() => {
    resizeObs?.disconnect();
    chart?.destroy();
  });
</script>

<!-- svelte-ignore a11y_click_events_have_key_events -->
<!-- svelte-ignore a11y_no_static_element_interactions -->
<div
  bind:this={wrapEl}
  class="rounded-lg overflow-hidden relative cursor-crosshair"
  style="background: var(--surface); border: 1px solid var(--border);"
  onclick={handleClick}
>
  <div bind:this={chartEl} class="w-full"></div>

  <!-- Playback cursor overlay -->
  <div
    bind:this={cursorEl}
    class="absolute top-0 bottom-0 pointer-events-none"
    style="width: 1px; background: rgba(255,255,255,0.5); opacity: 0; will-change: transform;"
  ></div>
</div>
