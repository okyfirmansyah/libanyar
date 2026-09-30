// Automated UI stress test for the video-player (injected by main.cpp when
// VIDEO_PLAYER_TEST_SCRIPT points at this file).  Drives the REAL UI:
// opens a file, presses ▶ with a real (GDK) click, then clicks random points
// on the bitrate chart / waveform with real clicks, and checks after each
// seek that the audio clock keeps advancing.  Run it via test/run_seek_stress.sh.
//
// Config (set by the runner before this file): window.__VP_TEST = { file, seeks, seed }

(function () {
  const cfg = window.__VP_TEST || {};
  const ipc = (cmd, args) => window.__anyar_ipc__(JSON.stringify({ id: 't' + Math.random(), cmd, args: args || {} }));
  const log = (msg) => ipc('test:log', { msg: typeof msg === 'string' ? msg : JSON.stringify(msg) });
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  let seed = cfg.seed || 12345;
  const rnd = () => (seed = (seed * 16807) % 2147483647) / 2147483647;

  async function waitFor(fn, timeoutMs, what) {
    const end = Date.now() + timeoutMs;
    while (Date.now() < end) { const v = fn(); if (v) return v; await sleep(50); }
    throw new Error('timeout waiting for ' + what);
  }

  function center(el) {
    const r = el.getBoundingClientRect();
    return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
  }

  async function realClick(x, y) {
    ipc('test:click', { x, y });
    await sleep(60);
  }

  // The bottom panel auto-hides while playing; a mousemove near the bottom shows it.
  async function revealPanel() {
    const content = document.querySelector('main > div.flex-1');
    const r = content.getBoundingClientRect();
    content.dispatchEvent(new MouseEvent('mousemove', { bubbles: true, clientX: r.left + r.width / 2, clientY: r.bottom - 20 }));
    await sleep(450);   // slide-in transition
  }

  async function run() {
    await waitFor(() => window.__videoPlayerTest && window.__anyar_ipc__, 10000, 'app');
    log('opening ' + cfg.file);
    window.__videoPlayerTest.open(cfg.file);
    const audio = await waitFor(() => document.querySelector('audio'), 15000, '<audio>');
    await waitFor(() => audio.readyState >= 1 && document.querySelector('.u-over'), 30000, 'metadata + chart');
    await sleep(500);

    const playBtn = [...document.querySelectorAll('button')].find((b) => b.textContent.trim() === '▶');
    const p = center(playBtn);
    await realClick(p.x, p.y);
    await waitFor(() => !audio.paused, 5000, 'audio playing after ▶ click');
    log('playing');
    await sleep(1500);

    const results = [];
    for (let i = 0; i < (cfg.seeks || 30); i++) {
      await revealPanel();
      const useChart = rnd() < 0.6;
      const target = useChart ? document.querySelector('.u-over')
                              : document.querySelector('[data-testid="waveform"]') || document.querySelector('.cursor-pointer');
      const r = target.getBoundingClientRect();
      const x = r.left + 2 + rnd() * (r.width - 4);
      const y = r.top + r.height / 2;
      await realClick(x, y);
      const clickAt = performance.now();
      const startPos = audio.currentTime;
      // Measure how long until the audio clock actually moves again.
      let startMs = -1;
      if (cfg.measureStart) {
        let seen = null;
        while (performance.now() - clickAt < 10000) {
          const ct = audio.currentTime;
          if (seen === null && Math.abs(ct - startPos) > 0.5) seen = ct;          // seek landed
          else if (seen !== null && ct - seen > 0.05) { startMs = Math.round(performance.now() - clickAt); break; }
          await sleep(20);
        }
        log({ seek: i, startMs, landed: seen, at: +audio.currentTime.toFixed(2), paused: audio.paused, rs: audio.readyState });
        continue;
      }
      const gap = 300 + Math.floor(rnd() * 2700);
      await sleep(gap);
      const a0 = audio.currentTime;
      const rec0 = window.__vpAudioRecoveries || 0;
      await sleep(cfg.window || 3500);   // long enough for the player's stall recovery
      const adv = audio.currentTime - a0;
      const recoveries = (window.__vpAudioRecoveries || 0) - rec0;
      // Reaching the end of the file legitimately stops playback: restart it.
      if (audio.paused && audio.duration - audio.currentTime < 1.5) {
        log({ seek: i, note: 'reached end of file — pressing play again' });
        const b = [...document.querySelectorAll('button')].find((x) => x.textContent.trim() === '▶');
        if (b) { const c = center(b); await realClick(c.x, c.y); await sleep(1500); }
        continue;
      }
      const ok = adv > 0.5;
      results.push(ok);
      log({ seek: i, on: useChart ? 'chart' : 'waveform', gap, audioAt: +a0.toFixed(2), audioAdvanced: +adv.toFixed(2),
            recoveries, paused: audio.paused, readyState: audio.readyState, ok });
    }
    const stuck = results.filter((x) => !x).length;
    log({ summary: true, seeks: results.length, audioStuck: stuck, recoveries: window.__vpAudioRecoveries || 0 });
    ipc('test:quit', { code: stuck ? 1 : 0 });
  }

  // Start once the native IPC binding exists (it may be installed after us).
  (async () => {
    while (typeof window.__anyar_ipc__ !== 'function') await sleep(50);
    log('driver loaded');
    try { await run(); } catch (e) { log('ERROR ' + e.message); ipc('test:quit', { code: 2 }); }
  })();
})();
