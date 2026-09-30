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
   *
   * Coordinates: uPlot's posToVal()/valToPos() work in CSS px relative to
   * the PLOT AREA (`chart.over`), not the chart root — the y-axis gutter
   * sits to the left of it.  The x scale is pinned to [0, duration] so the
   * axis, the cursor and click-to-seek all share one mapping.
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
  let totalWidth = 0;
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
    totalWidth = chart.width;
    geoSeq++;                       // nudge cursor $effect
    onlayout?.(plotLeft, plotWidth, totalWidth); // safe — not inside an $effect
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
        // Fixed to the media timeline (not the data extent, which ends
        // half a bucket early and would skew every position).
        x: { time: false, range: (u, min, max) => [0, duration > 0 ? duration : max] },
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
          stroke: '#6a8595',
          fill: 'rgba(106, 133, 149, 0.12)',
          width: 1.5,
          paths: uPlot.paths.bars({ size: [0.6, 100] }),
        },
        {
          label: 'Audio',
          stroke: '#a3965c',
          fill: 'rgba(163, 150, 92, 0.12)',
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
    totalWidth = c.width;

    // Notify parent via microtask (outside this $effect's tracking scope)
    queueMicrotask(() => {
      onlayout?.(plotLeft, plotWidth, totalWidth);
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

  // ── Re-pin the x scale when the duration becomes known / changes ───
  $effect(() => {
    const d = duration;
    const _ = geoSeq;
    if (chart && d > 0) chart.setScale('x', { min: 0, max: d });
  });

  // ── Position cursor overlay ────────────────────────────────────────
  // Same mapping as the axis: valToPos() is plot-relative, so offset by
  // the plot's left edge within the chart.
  // Tracked deps: currentTime, cursorEl, geoSeq, duration.
  $effect(() => {
    const t = currentTime;
    const el = cursorEl;
    const _ = geoSeq;          // re-run when chart geometry changes

    if (!el || !chart || plotWidth <= 0 || duration <= 0) {
      if (el) el.style.opacity = '0';
      return;
    }

    const clamped = Math.max(0, Math.min(t, duration));
    const x = plotLeft + chart.valToPos(clamped, 'x');
    el.style.transform = `translateX(${x}px)`;
    el.style.opacity = '1';
  });

  // Click to seek — measure from the plot area, which is what posToVal expects.
  function handleClick(e) {
    if (!chart || duration <= 0 || !onseek) return;
    const over = chart.over.getBoundingClientRect();
    const x = e.clientX - over.left;
    if (x < 0 || x > over.width) return;   // click on the axis gutter
    const time = chart.posToVal(x, 'x');
    onseek(Math.max(0, Math.min(time, duration)));
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
