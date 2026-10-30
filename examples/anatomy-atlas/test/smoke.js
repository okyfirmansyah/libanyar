// Anatomy Atlas smoke test — runs inside the real webview (docs/debugging.md).
//
//   xvfb-run -a env GDK_BACKEND=x11 ANYAR_TEST_SCRIPT=examples/anatomy-atlas/test/smoke.js \
//       ANYAR_TEST_TIMEOUT=120 ./build/examples/anatomy-atlas/anatomy_atlas
//
// Exit code 0 = pass. Screenshots go to ./atlas-smoke/ when the platform can
// capture (Linux today); input steps are skipped where unsupported.
const caps = await anyarTest.info();
console.log('platform:', caps.platform, 'capture:', caps.capture, 'input:', caps.input);

await anyarTest.waitFor(() => document.getElementById('loading').classList.contains('done'), 90000);
console.log('loaded:', document.getElementById('packChip').textContent, '·', document.getElementById('status').textContent);
if (caps.capture) await anyarTest.screenshot('atlas-smoke/01-loaded.png');

if (caps.input) {
  // Search with real keyboard input, pick the first result with Enter.
  await anyarTest.click('#search');
  await anyarTest.type('left femur');
  await anyarTest.waitFor(() => document.querySelectorAll('#results button').length > 0, 5000);
  await anyarTest.key('Enter');
} else {
  // Same flow through the DOM where synthetic input isn't available yet.
  const s = document.getElementById('search');
  s.value = 'left femur';
  s.dispatchEvent(new Event('input'));
  await anyarTest.waitFor(() => document.querySelectorAll('#results button').length > 0, 5000);
  document.querySelector('#results button').click();
}
await anyarTest.waitFor(() => !document.getElementById('info').hidden, 5000);
const selected = document.querySelector('#info h3').textContent;
anyarTest.assert(selected === 'Left femur', 'selected ' + selected);
console.log('selected:', selected);

// Sections and X-ray toggle without errors.
document.getElementById('clipBtn').click();
document.getElementById('xrayBtn').click();
await anyarTest.sleep(1500);
if (caps.capture) await anyarTest.screenshot('atlas-smoke/02-section-xray.png');

await anyarTest.done(0);
