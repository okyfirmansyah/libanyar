// Anatomy Atlas — app shell: systems, tree, search, selection, streaming.

import './style.css';
import * as THREE from 'three';
import { buffer, invoke } from '@libanyar/api';
import { AtlasView, type ClipAxis } from './atlas';
import { DEFAULT_ON, REGION_LABEL, SYSTEM_COLOR, SYSTEM_LABEL, partColor } from './palette';
import type { AtlasInfo, GeometryMeta, Part, PartMode } from './types';

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const esc = (s: string) =>
  s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);
const fmt = (n: number) => n.toLocaleString('en-US');
const mb = (b: number) => `${(b / 1048576).toFixed(1)} MB`;

const EYE = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/></svg>';
const EYE_OFF = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 3l18 18M10.6 5.1A10.9 10.9 0 0 1 12 5c6.4 0 10 7 10 7a17.6 17.6 0 0 1-3.2 4.1M6.6 6.6C3.9 8.3 2 12 2 12s3.6 7 10 7c1.9 0 3.6-.6 5-1.5"/></svg>';
const GHOST = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 21V10a7 7 0 0 1 14 0v11l-3-2-2 2-2-2-2 2-2-2z"/><circle cx="9.5" cy="10.5" r="1"/><circle cx="14.5" cy="10.5" r="1"/></svg>';

const view = new AtlasView($('viewport'));

let info: AtlasInfo;
let parts: Part[] = [];
const bySystem = new Map<string, Part[]>();
const systemsOn = new Set<string>();
const systemsGhost = new Set<string>();
const hiddenParts = new Set<number>();
let isolated: number | null = null;
let xray = false;
let selected = -1;
let hovered = -1;
let anchor: THREE.Vector3 | null = null;
let lastStream = '';
const clip = { on: false, axis: 'x' as ClipAxis, pos: 0, flip: false };

// ── Streaming ────────────────────────────────────────────────────────────────

let queue: Promise<void> = Promise.resolve();

/** Serialise backend loads so memory stays bounded and the status line is readable. */
function enqueue(job: () => Promise<void>) {
  queue = queue.then(job).catch((e) => console.error('[atlas]', e));
  return queue;
}

/** Fetch one geometry SharedBuffer, upload it, and let C++ free it. */
async function fetchGeometry(cmd: string, args: object): Promise<{ meta: GeometryMeta; bytes: ArrayBuffer; fetchMs: number }> {
  const meta = await invoke<GeometryMeta>(cmd, args);
  const t0 = performance.now();
  const bytes = await buffer.fetchBuffer(meta.buffer, { id: meta.id });
  const fetchMs = performance.now() - t0;
  void invoke('atlas:release', { buffer: meta.buffer });
  return { meta, bytes, fetchMs };
}

const coarseLod = () => info.lods - 1;
const workLod = () => (info.lods >= 3 ? 1 : coarseLod());
const lodLabel = (lod: number) => (info.lods_mm[lod] ? `${info.lods_mm[lod]} mm` : 'full');

/** Load a system coarse first, then stream the working LOD over it. */
function ensureSystem(system: string) {
  for (const lod of new Set([coarseLod(), workLod()])) {
    enqueue(async () => {
      const have = view.loadedLod(system);
      if (have !== undefined && have <= lod) return;
      if (!systemsOn.has(system) && have !== undefined) return;
      const { meta, bytes, fetchMs } = await fetchGeometry('atlas:load', { system, lod });
      view.setGeometry(system, lod, meta, bytes);
      lastStream = `${SYSTEM_LABEL[system]} @ ${lodLabel(lod)}: ${fmt(meta.triangles)} ▲, ${mb(meta.bytes)} — C++ ${meta.ms} ms, SharedBuffer read ${fetchMs.toFixed(0)} ms`;
      renderSystems();
    });
  }
}

let detailToken = 0;
async function loadDetail(index: number) {
  const token = ++detailToken;
  view.setDetailPart(-1);
  if (info.lods < 2 || index < 0) return;
  const { meta, bytes, fetchMs } = await fetchGeometry('atlas:load_parts', { parts: [index], lod: 0 });
  if (token !== detailToken) return;            // selection changed meanwhile
  view.setGeometry('detail', 0, meta, bytes);
  view.setDetailPart(index);
  lastStream = `Full detail: ${parts[index].name} — ${fmt(meta.triangles)} ▲, C++ ${meta.ms} ms, read ${fetchMs.toFixed(0)} ms`;
  renderInfo();
}

// ── Visibility ───────────────────────────────────────────────────────────────

function modeOf(p: Part): PartMode {
  if (!systemsOn.has(p.system) || hiddenParts.has(p.index)) return 'hidden';
  if (isolated !== null) return p.index === isolated ? 'visible' : 'ghost';
  if (xray && selected >= 0) return p.index === selected ? 'visible' : 'ghost';
  if (systemsGhost.has(p.system)) return 'ghost';
  return 'visible';
}

function applyModes() {
  for (const p of parts) view.setMode(p.index, modeOf(p));
  renderSystems();
  refreshTreeLeaves();
}

// ── Selection ────────────────────────────────────────────────────────────────

function select(index: number, opts: { frame?: boolean; at?: { x: number; y: number } } = {}) {
  if (selected >= 0) view.setHighlight(selected, 0);
  if (isolated !== null && index !== isolated) isolated = null;
  selected = index;
  anchor = null;
  if (index >= 0) {
    const p = parts[index];
    if (!systemsOn.has(p.system)) {
      systemsOn.add(p.system);
      ensureSystem(p.system);
    }
    hiddenParts.delete(index);
    view.setHighlight(index, 2);
    if (opts.at) anchor = view.surfacePoint(index, opts.at.x, opts.at.y);
    if (!anchor) anchor = new THREE.Vector3(...p.min).add(new THREE.Vector3(...p.max)).multiplyScalar(0.5);
    if (opts.frame) view.frame(new THREE.Vector3(...p.min), new THREE.Vector3(...p.max), 'keep');
    void loadDetail(index);
  } else {
    void loadDetail(-1);
  }
  applyModes();
  renderInfo();
  revealInTree(index);
}

function renderInfo() {
  const el = $('info');
  if (selected < 0) {
    el.hidden = true;
    return;
  }
  const p = parts[selected];
  const tri = p.triangles.map((t, i) => `${fmt(t)} @ ${lodLabel(i)}`).join(' · ');
  const detail = info.lods > 1 && view.loadedLod('detail') === 0 ? '<div class="detail">Showing full-resolution mesh</div>' : '';
  el.innerHTML = `
    <button class="icon close" data-act="close" title="Close">✕</button>
    <h3>${esc(p.name)}</h3>
    <dl class="meta">
      <dt>System</dt><dd><span style="color:${SYSTEM_COLOR[p.system]}">●</span> ${SYSTEM_LABEL[p.system] ?? p.system}</dd>
      <dt>Type</dt><dd>${esc(p.kind)}</dd>
      <dt>Region</dt><dd>${REGION_LABEL[p.region] ?? p.region}</dd>
      <dt>FMA</dt><dd>${esc(p.fma || '—')}</dd>
      <dt>Part id</dt><dd>${esc(p.id)}</dd>
      <dt>Triangles</dt><dd>${tri}</dd>
    </dl>
    <div class="actions">
      <button class="btn small" data-act="frame">Frame</button>
      <button class="btn small" data-act="isolate" aria-pressed="${isolated === selected}">${isolated === selected ? 'Show all' : 'Isolate'}</button>
      <button class="btn small" data-act="hide">Hide</button>
    </div>${detail}`;
  el.hidden = false;
}

$('info').addEventListener('click', (e) => {
  const act = (e.target as HTMLElement).closest<HTMLElement>('[data-act]')?.dataset.act;
  if (!act || selected < 0) return;
  const p = parts[selected];
  if (act === 'close') select(-1);
  else if (act === 'frame') view.frame(new THREE.Vector3(...p.min), new THREE.Vector3(...p.max), 'keep');
  else if (act === 'isolate') {
    isolated = isolated === selected ? null : selected;
    applyModes();
    renderInfo();
    if (isolated !== null) view.frame(new THREE.Vector3(...p.min), new THREE.Vector3(...p.max), 'keep');
  } else if (act === 'hide') {
    hiddenParts.add(selected);
    select(-1);
  }
});

// ── Sidebar: systems ─────────────────────────────────────────────────────────

function renderSystems() {
  $('systems').innerHTML = info.systems
    .filter((s) => bySystem.has(s))
    .map((s) => {
      const on = systemsOn.has(s), ghost = systemsGhost.has(s);
      const loaded = view.loadedLod(s);
      return `<li data-system="${s}">
        <button class="icon ${on ? '' : 'off'}" data-act="toggle" title="${on ? 'Hide' : 'Show'}">${on ? EYE : EYE_OFF}</button>
        <span class="sw" style="background:${SYSTEM_COLOR[s]}"></span>
        <span>${SYSTEM_LABEL[s] ?? s}</span>
        <span class="count">${on && loaded !== undefined ? `<span class="state">${lodLabel(loaded)}</span> · ` : ''}${bySystem.get(s)!.length}</span>
        <button class="icon ${ghost ? 'on-ghost' : ''}" data-act="ghost" title="X-ray this system">${GHOST}</button>
      </li>`;
    })
    .join('');
}

$('systems').addEventListener('click', (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLElement>('[data-act]');
  const s = btn?.closest<HTMLElement>('[data-system]')?.dataset.system;
  if (!btn || !s) return;
  if (btn.dataset.act === 'toggle') {
    if (systemsOn.has(s)) systemsOn.delete(s);
    else {
      systemsOn.add(s);
      ensureSystem(s);
    }
  } else if (systemsGhost.has(s)) systemsGhost.delete(s);
  else systemsGhost.add(s);
  applyModes();
});

// ── Sidebar: tree (System → Region → parts, leaves built on open) ───────────

function buildTree() {
  const html = info.systems
    .filter((s) => bySystem.has(s))
    .map((s) => {
      const regions = new Map<string, Part[]>();
      for (const p of bySystem.get(s)!) regions.set(p.region, [...(regions.get(p.region) ?? []), p]);
      const order = Object.keys(REGION_LABEL).filter((r) => regions.has(r));
      return `<details data-system="${s}"><summary><span style="color:${SYSTEM_COLOR[s]}">●</span>${SYSTEM_LABEL[s] ?? s}<small>${bySystem.get(s)!.length}</small></summary>
        ${order.map((r) => `<details class="lvl2" data-region="${r}"><summary>${REGION_LABEL[r]}<small>${regions.get(r)!.length}</small></summary><div class="leaves"></div></details>`).join('')}
      </details>`;
    })
    .join('');
  $('tree').innerHTML = html;
  $('browseCount').textContent = `${fmt(parts.length)} parts`;
}

function leavesHtml(system: string, region: string) {
  return bySystem
    .get(system)!
    .filter((p) => p.region === region)
    .sort((a, b) => a.name.localeCompare(b.name))
    .map((p) => {
      const hidden = view.mode(p.index) === 'hidden';
      return `<div class="leaf ${p.index === selected ? 'sel' : ''} ${hidden ? 'hidden' : ''}" data-part="${p.index}">
        <span title="${esc(p.name)}">${esc(p.name)}</span>
        <button class="icon ${hiddenParts.has(p.index) ? 'off' : ''}" data-act="eye">${hiddenParts.has(p.index) ? EYE_OFF : EYE}</button></div>`;
    })
    .join('');
}

$('tree').addEventListener('toggle', (e) => {
  const d = e.target as HTMLDetailsElement;
  if (!d.open || !d.dataset.region) return;
  const s = d.closest<HTMLElement>('[data-system]')!.dataset.system!;
  d.querySelector('.leaves')!.innerHTML = leavesHtml(s, d.dataset.region);
}, true);

function refreshTreeLeaves() {
  for (const d of document.querySelectorAll<HTMLDetailsElement>('#tree details.lvl2[open]')) {
    const s = d.closest<HTMLElement>('[data-system]')!.dataset.system!;
    d.querySelector('.leaves')!.innerHTML = leavesHtml(s, d.dataset.region!);
  }
}

function revealInTree(index: number) {
  if (index < 0) return;
  const p = parts[index];
  const sys = document.querySelector<HTMLDetailsElement>(`#tree details[data-system="${p.system}"]`);
  const reg = sys?.querySelector<HTMLDetailsElement>(`details[data-region="${p.region}"]`);
  if (!sys || !reg) return;
  sys.open = true;
  if (!reg.open) reg.open = true;      // the toggle listener fills the leaves
  else refreshTreeLeaves();
  requestAnimationFrame(() => reg.querySelector(`[data-part="${index}"]`)?.scrollIntoView({ block: 'nearest' }));
}

$('tree').addEventListener('click', (e) => {
  const leaf = (e.target as HTMLElement).closest<HTMLElement>('[data-part]');
  if (!leaf) return;
  const i = Number(leaf.dataset.part);
  if ((e.target as HTMLElement).closest('[data-act="eye"]')) {
    if (hiddenParts.has(i)) hiddenParts.delete(i);
    else {
      hiddenParts.add(i);
      if (i === selected) select(-1);
    }
    applyModes();
  } else {
    select(i, { frame: true });
  }
});

// ── Search ───────────────────────────────────────────────────────────────────

let searchTimer: number | undefined;
let results: number[] = [];
let active = 0;

function renderResults(ms?: number) {
  const box = $('results');
  const q = $<HTMLInputElement>('search').value.trim();
  if (!q) {
    box.classList.remove('on');
    return;
  }
  box.innerHTML = results.length
    ? results
        .map((i, k) => {
          const p = parts[i];
          return `<button data-part="${i}" class="${k === active ? 'active' : ''}" role="option">
            <span style="color:${SYSTEM_COLOR[p.system]}">●</span>${esc(p.name)}<small>${REGION_LABEL[p.region] ?? ''} · ${SYSTEM_LABEL[p.system]}</small></button>`;
        })
        .join('') + (ms !== undefined ? `<div class="empty">C++ search: ${ms} ms</div>` : '')
    : '<div class="empty">No matching structure</div>';
  box.classList.add('on');
}

$('search').addEventListener('input', () => {
  window.clearTimeout(searchTimer);
  searchTimer = window.setTimeout(async () => {
    const q = $<HTMLInputElement>('search').value.trim();
    if (!q) {
      results = [];
      renderResults();
      return;
    }
    const r = await invoke<{ results: number[]; ms: number }>('atlas:search', { q, limit: 14 });
    results = r.results;
    active = 0;
    renderResults(r.ms);
  }, 110);
});

$('search').addEventListener('keydown', (e) => {
  if (e.key === 'ArrowDown') active = Math.min(active + 1, results.length - 1);
  else if (e.key === 'ArrowUp') active = Math.max(active - 1, 0);
  else if (e.key === 'Enter' && results.length) {
    pickResult(results[active]);
    return;
  } else if (e.key === 'Escape') {
    $('results').classList.remove('on');
    return;
  } else return;
  e.preventDefault();
  renderResults();
});

$('results').addEventListener('click', (e) => {
  const b = (e.target as HTMLElement).closest<HTMLElement>('[data-part]');
  if (b) pickResult(Number(b.dataset.part));
});

function pickResult(i: number) {
  $('results').classList.remove('on');
  $<HTMLInputElement>('search').blur();
  select(i, { frame: true });
}

document.addEventListener('click', (e) => {
  if (!(e.target as HTMLElement).closest('.search')) $('results').classList.remove('on');
});

// ── Viewport interaction ─────────────────────────────────────────────────────

const canvas = view.renderer.domElement;
let down: { x: number; y: number } | null = null;
let hoverTimer = 0;

canvas.addEventListener('pointerdown', (e) => (down = { x: e.offsetX, y: e.offsetY }));
canvas.addEventListener('pointerup', (e) => {
  if (!down || Math.hypot(e.offsetX - down.x, e.offsetY - down.y) > 4) return;
  const i = view.pick(e.offsetX, e.offsetY);
  select(i, { at: { x: e.offsetX, y: e.offsetY } });
});
canvas.addEventListener('dblclick', (e) => {
  const i = view.pick(e.offsetX, e.offsetY);
  if (i >= 0) select(i, { frame: true, at: { x: e.offsetX, y: e.offsetY } });
});
canvas.addEventListener('pointermove', (e) => {
  if (e.buttons) return;
  const now = performance.now();
  if (now - hoverTimer < 80) return;
  hoverTimer = now;
  const i = view.pick(e.offsetX, e.offsetY);
  if (i !== hovered) {
    if (hovered >= 0 && hovered !== selected) view.setHighlight(hovered, 0);
    hovered = i;
    if (i >= 0 && i !== selected) view.setHighlight(i, 1);
  }
  const tip = $('tooltip');
  tip.hidden = i < 0;
  if (i >= 0) {
    tip.textContent = parts[i].name;
    tip.style.left = `${e.offsetX + 14}px`;
    tip.style.top = `${e.offsetY + 12}px`;
  }
});
canvas.addEventListener('pointerleave', () => {
  $('tooltip').hidden = true;
  if (hovered >= 0 && hovered !== selected) view.setHighlight(hovered, 0);
  hovered = -1;
});

// Selection label + status line, every frame.
const pin = document.createElement('div');
pin.className = 'pin';
pin.innerHTML = '<svg width="1" height="1"><line stroke="var(--accent)" stroke-width="1.5"/></svg><i></i><span></span>';
$('labels').appendChild(pin);
let statusT = 0;
view.onFrame = () => {
  const s = anchor && selected >= 0 ? view.project(anchor) : null;
  pin.style.display = s ? '' : 'none';
  if (s) {
    const w = $('viewport').clientWidth;
    const dx = s.x > w * 0.6 ? -70 : 70, dy = -46;
    pin.style.transform = `translate(${s.x}px, ${s.y}px)`;
    const line = pin.querySelector('line')!;
    line.setAttribute('x2', String(dx));
    line.setAttribute('y2', String(dy));
    const tag = pin.querySelector('span')!;
    tag.textContent = parts[selected].name;
    tag.style.transform = `translate(${dx < 0 ? 'calc(-100% + ' + dx + 'px)' : dx + 'px'}, calc(${dy}px - 50%))`;
  }
  const now = performance.now();
  if (now - statusT > 500) {
    statusT = now;
    $('status').textContent = `${info?.pack === 'full' ? 'Full pack' : 'Sample pack'} · ${fmt(view.triangles)} ▲ on GPU · ${view.fps} fps${lastStream ? ' · ' + lastStream : ''}`;
  }
};

// ── Toolbar ──────────────────────────────────────────────────────────────────

function regionBox(region: string | null): [THREE.Vector3, THREE.Vector3] {
  const min = new THREE.Vector3(Infinity, Infinity, Infinity), max = new THREE.Vector3(-Infinity, -Infinity, -Infinity);
  for (const p of parts) {
    if (region && p.region !== region) continue;
    if (p.system === 'integumentary' && region) continue;
    min.min(new THREE.Vector3(...p.min));
    max.max(new THREE.Vector3(...p.max));
  }
  return [min, max];
}

function renderRegions() {
  $('regions').innerHTML = [['', 'Full body'], ...Object.entries(REGION_LABEL).filter(([r]) => r !== 'whole_body')]
    .map(([r, label]) => `<button class="btn" data-region="${r}">${label}</button>`)
    .join('');
}

let lastRegion: string | null = null;
$('regions').addEventListener('click', (e) => {
  const b = (e.target as HTMLElement).closest<HTMLElement>('[data-region]');
  if (!b) return;
  lastRegion = b.dataset.region || null;
  const [min, max] = regionBox(lastRegion);
  view.frame(min, max, 'front');
});

$('xrayBtn').addEventListener('click', () => {
  xray = !xray;
  $('xrayBtn').setAttribute('aria-pressed', String(xray));
  applyModes();
});

$('resetBtn').addEventListener('click', () => {
  systemsOn.clear();
  for (const s of info.systems) if (DEFAULT_ON.has(s) && bySystem.has(s)) { systemsOn.add(s); ensureSystem(s); }
  systemsGhost.clear();
  hiddenParts.clear();
  isolated = null;
  xray = false;
  $('xrayBtn').setAttribute('aria-pressed', 'false');
  select(-1);
  const [min, max] = regionBox(null);
  view.frame(min, max, 'front');
});

function applyClip(turn = false) {
  const [min, max] = regionBox(null);
  const a = clip.axis;
  const value = min[a] + ((clip.pos + 1) / 2) * (max[a] - min[a]);
  view.setClip(clip.on, clip.axis, value, clip.flip);
  if (clip.on && turn) {
    // Look at the cut face: from the removed side, slightly raised.
    const s = clip.flip ? -1 : 1;
    const dir = { x: new THREE.Vector3(s, 0.2, 0.45), y: new THREE.Vector3(0.15, s, 0.6), z: new THREE.Vector3(0.25, 0.2, s) }[a];
    const [rmin, rmax] = regionBox(lastRegion);
    view.frame(rmin, rmax, dir);
  }
}

$('clipBtn').addEventListener('click', () => {
  clip.on = !clip.on;
  $('clipBtn').setAttribute('aria-pressed', String(clip.on));
  $('clipbar').hidden = !clip.on;
  applyClip(true);
});
$('clipAxis').addEventListener('click', (e) => {
  const b = (e.target as HTMLElement).closest<HTMLElement>('[data-axis]');
  if (!b) return;
  clip.axis = b.dataset.axis as ClipAxis;
  for (const x of $('clipAxis').querySelectorAll('button')) x.setAttribute('aria-pressed', String(x === b));
  applyClip(true);
});
$('clipPos').addEventListener('input', (e) => {
  clip.pos = Number((e.target as HTMLInputElement).value);
  applyClip();
});
$('clipFlip').addEventListener('click', () => {
  clip.flip = !clip.flip;
  applyClip(true);
});

// ── About & credits (generated from the pack's manifest, not hard-coded) ────

function renderCredits() {
  const s = info.source;
  $('credit').innerHTML = `${esc(s.attribution)} · <a href="${s.license_url}" target="_blank" rel="noreferrer">${esc(s.license)}</a>
    · For education only, not for clinical use · <button id="creditAbout">About</button>`;
  const m = (info.manifest ?? {}) as Record<string, unknown>;
  const out = (m.output ?? {}) as Record<string, unknown>;
  $('aboutBody').innerHTML = `
    <h3>Anatomy Atlas</h3>
    <p>A LibAnyar example: a C++ plugin decodes a whole-body anatomy pack and streams it, system by system and level by level,
    to three.js through SharedBuffers.</p>
    <div class="warn"><b>Not a medical device.</b> For education and demonstration only. Do not use for diagnosis or treatment.</div>
    <h4>Anatomy data</h4>
    <blockquote>${esc(s.attribution)}</blockquote>
    <ul>
      <li>Licence: <a href="${s.license_url}" target="_blank" rel="noreferrer">${esc(s.license)}</a>
        (<a href="${s.license_page ?? s.source_url}" target="_blank" rel="noreferrer">licence page</a>)</li>
      <li>Source: <a href="${s.source_url}" target="_blank" rel="noreferrer">${esc(s.name)} ${esc(s.version)}</a>, retrieved ${esc(String(m.retrieved ?? '—'))}</li>
      <li>Changes: ${esc(String(m.changes ?? 'converted and simplified'))}</li>
      <li>Pack: ${info.pack === 'full' ? 'full (3 levels of detail)' : 'embedded sample (12 mm)'}, ${fmt(info.parts)} parts${out.sha256 ? `, SHA-256 <code>${esc(String(out.sha256))}</code>` : ''}</li>
    </ul>
    ${m.note ? `<p class="muted">${esc(String(m.note))}</p>` : ''}
    <h4>Software</h4>
    <ul>
      <li>LibAnyar — MIT License</li>
      <li>three.js — MIT License</li>
      <li>Third-party components: see <code>NOTICE</code> in the repository.</li>
    </ul>`;
}

const showAbout = () => $<HTMLDialogElement>('about').showModal();
$('aboutBtn').addEventListener('click', showAbout);
$('credit').addEventListener('click', (e) => {
  if ((e.target as HTMLElement).id === 'creditAbout') showAbout();
});

// ── Boot ─────────────────────────────────────────────────────────────────────

async function boot() {
  $('loadText').textContent = 'Reading anatomy pack…';
  info = await invoke<AtlasInfo>('atlas:info');
  const table = await invoke<{ fields: string[]; parts: unknown[][] }>('atlas:parts');
  parts = table.parts.map((r, index) => ({
    index, id: r[0] as string, fma: r[1] as string, name: r[2] as string, system: r[3] as string,
    kind: r[4] as string, region: r[5] as string,
    min: [r[6], r[7], r[8]] as [number, number, number], max: [r[9], r[10], r[11]] as [number, number, number],
    triangles: r[12] as number[],
  }));
  for (const p of parts) bySystem.set(p.system, [...(bySystem.get(p.system) ?? []), p]);
  view.setParts(parts, (p) => partColor(p.system, p.kind, p.id));

  const chip = $('packChip');
  chip.textContent = info.pack === 'full' ? `Full pack · ${info.lods} LODs` : 'Sample pack (12 mm)';
  chip.classList.toggle('full', info.pack === 'full');
  renderCredits();
  renderRegions();
  buildTree();

  for (const s of info.systems) if (DEFAULT_ON.has(s) && bySystem.has(s)) systemsOn.add(s);
  applyModes();
  const [min, max] = regionBox(null);
  view.frame(min, max, 'front', true);

  // Coarse pass for every default system first (quick first picture) …
  const on = [...systemsOn];
  let done = 0;
  for (const s of on)
    enqueue(async () => {
      const { meta, bytes, fetchMs } = await fetchGeometry('atlas:load', { system: s, lod: coarseLod() });
      view.setGeometry(s, coarseLod(), meta, bytes);
      lastStream = `${SYSTEM_LABEL[s]} @ ${lodLabel(coarseLod())}: ${fmt(meta.triangles)} ▲ — C++ ${meta.ms} ms, read ${fetchMs.toFixed(0)} ms`;
      $('loadBar').style.width = `${(++done / on.length) * 100}%`;
      $('loadText').textContent = `Streaming ${SYSTEM_LABEL[s]}…`;
      renderSystems();
    });
  await queue;
  $('loading').classList.add('done');
  // … then stream the finer level for the visible systems in the background.
  for (const s of on) ensureSystem(s);
  await queue;
  renderSystems();
}

boot().catch((e) => {
  $('loadText').textContent = `Failed to load anatomy: ${e instanceof Error ? e.message : String(e)}`;
  console.error(e);
});
