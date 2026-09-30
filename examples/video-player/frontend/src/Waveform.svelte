<script>
  /**
   * Waveform — Audio waveform visualisation using wavesurfer.js.
   * Renders pre-computed peaks from the C++ backend.
   * Click to seek; cursor syncs with video currentTime.
   */
  import WaveSurfer from 'wavesurfer.js';
  import { onDestroy } from 'svelte';

  let { peaks = [], duration = 0, currentTime = 0, onseek, padLeft = 0, padRight = 0 } = $props();

  let wrapEl = $state(null);
  let wsContainer = $state(null);
  let ws = null;

  // Create WaveSurfer when container and peaks are ready
  $effect(() => {
    if (!wsContainer || !peaks || peaks.length === 0 || duration <= 0) return;

    // Destroy previous instance if exists
    if (ws) {
      ws.destroy();
      ws = null;
    }

    ws = WaveSurfer.create({
      container: wsContainer,
      waveColor: 'rgba(163, 150, 92, 0.35)',
      progressColor: 'rgba(163, 150, 92, 0.75)',
      cursorColor: 'rgba(228, 230, 231, 0.5)',
      cursorWidth: 1,
      height: 64,
      barWidth: 2,
      barGap: 1,
      barRadius: 1,
      normalize: true,
      interact: false,       // We handle clicks manually to avoid event loops
      hideScrollbar: true,
      peaks: [peaks],
      duration: duration,
    });
  });

  // Sync progress cursor from video's currentTime
  $effect(() => {
    if (ws && duration > 0) {
      const progress = Math.min(Math.max(currentTime / duration, 0), 1);
      try {
        ws.seekTo(progress);
      } catch (_) {
        // Ignore errors during transitions
      }
    }
  });

  // Click-to-seek handler
  function handleClick(e) {
    if (!wrapEl || !wsContainer || duration <= 0 || !onseek) return;
    // Map over the drawn area only (the paddings mirror the bitrate
    // chart's plot area so both timelines line up pixel-for-pixel).
    const inner = wsContainer.getBoundingClientRect();
    const clickX = e.clientX - inner.left;
    const drawWidth = inner.width;
    if (drawWidth <= 0) return;
    const progress = clickX / drawWidth;
    const time = Math.max(0, Math.min(progress * duration, duration));
    onseek(time);
  }

  onDestroy(() => {
    if (ws) {
      ws.destroy();
      ws = null;
    }
  });
</script>

<!-- svelte-ignore a11y_click_events_have_key_events -->
<!-- svelte-ignore a11y_no_static_element_interactions -->
<div
  bind:this={wrapEl}
  data-testid="waveform"
  class="rounded-lg cursor-pointer overflow-hidden relative"
  style="background: var(--surface); border: 1px solid var(--border); padding: 6px 0; padding-left: {padLeft}px; padding-right: {padRight}px;"
  onclick={handleClick}
>
  <div bind:this={wsContainer}></div>
</div>
