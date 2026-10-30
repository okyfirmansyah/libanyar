// Anatomy Atlas demo tour — drives the real app for a screen recording
// (docs/assets/anatomy-atlas.gif). Needs synthetic input (Linux today).
//
//   tools/record_demo.sh   # Xvfb + ffmpeg wrapper that runs this script
//
// Logs "DEMO_START <epoch ms>" / "DEMO_END <epoch ms>" so the recorder can trim.
const caps = await anyarTest.info();
anyarTest.assert(caps.input, 'demo needs synthetic input');

const $ = (id) => document.getElementById(id);
await anyarTest.waitFor(() => $('loading').classList.contains('done'), 90000);
await anyarTest.sleep(2500);                                   // finer LODs stream in

const vp = $('viewport').getBoundingClientRect();
const cx = vp.left + vp.width / 2, cy = vp.top + vp.height / 2;

async function drag(dx, dy, ms) {
  const steps = Math.round(ms / 30);
  await anyarTest.down({ x: cx, y: cy });
  for (let i = 1; i <= steps; i++) {
    await anyarTest.move({ x: cx + (dx * i) / steps, y: cy + (dy * i) / steps });
    await anyarTest.sleep(30);
  }
  await anyarTest.up({ x: cx + dx, y: cy + dy });
}
const system = (s, act = 'toggle') => anyarTest.click(`#systems [data-system="${s}"] [data-act="${act}"]`);

console.log('DEMO_START', Date.now());
await anyarTest.sleep(1200);

await drag(420, 0, 1800);                                      // orbit around the skeleton
await anyarTest.sleep(600);
await drag(-420, 0, 1200);
await anyarTest.sleep(500);

await system('muscular');                                      // add the muscles…
await anyarTest.sleep(2200);
await system('muscular', 'ghost');                             // …then X-ray them
await anyarTest.sleep(1600);
await system('muscular');
await anyarTest.sleep(400);

await anyarTest.click('#regions [data-region="thorax"]');      // fly to the chest
await anyarTest.sleep(1200);

await anyarTest.click('#search');                              // search + select with real typing
await anyarTest.type('heart');
await anyarTest.waitFor(() => document.querySelectorAll('#results button').length > 0, 5000);
await anyarTest.sleep(500);
await anyarTest.key('Enter');
await anyarTest.waitFor(() => !$('info').hidden, 5000);
console.log('selected:', document.querySelector('#info h3').textContent);
await anyarTest.sleep(900);
await anyarTest.click('#xrayBtn');
await anyarTest.sleep(1800);
await anyarTest.click('#xrayBtn');

await anyarTest.click('#clipBtn');                             // sweep a sagittal section
const pos = $('clipPos');
for (let v = -0.3; v <= 0.3; v += 0.02) {
  pos.value = String(v);
  pos.dispatchEvent(new Event('input', { bubbles: true }));
  await anyarTest.sleep(70);
}
await anyarTest.sleep(800);
await anyarTest.click('#clipBtn');

await anyarTest.click('#resetBtn');
await anyarTest.sleep(1500);
console.log('DEMO_END', Date.now());
await anyarTest.done(0);
