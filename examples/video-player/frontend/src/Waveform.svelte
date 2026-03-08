<script>
  /**
   * Waveform — Audio waveform visualisation using wavesurfer.js.
   * Renders pre-computed peaks from the C++ backend.
   * Click to seek; cursor syncs with video currentTime.
   */
  import WaveSurfer from 'wavesurfer.js';
  import { onDestroy } from 'svelte';

  let { peaks = [], duration = 0, currentTime = 0, onseek, padLeft = 0 } = $props();

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
      waveColor: 'rgba(6, 182, 212, 0.35)',
      progressColor: 'rgba(6, 182, 212, 0.75)',
      cursorColor: 'rgba(255, 255, 255, 0.6)',
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
    if (!wrapEl || duration <= 0 || !onseek) return;
    const rect = wrapEl.getBoundingClientRect();
    // Offset by padLeft so clicks map to the correct timeline position
    const clickX = e.clientX - rect.left - padLeft;
    const drawWidth = rect.width - padLeft;
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
  class="rounded-lg cursor-pointer overflow-hidden relative"
  style="background: var(--surface); border: 1px solid var(--border); padding: 6px 0; padding-left: {padLeft}px;"
  onclick={handleClick}
>
  <div bind:this={wsContainer}></div>
</div>
