// AtlasView — three.js renderer for the whole-body atlas.
//
// Geometry arrives per body system as one merged mesh (from a SharedBuffer),
// with a `partIdx` attribute on every vertex.  Per-part state lives in two
// small GPU textures, so hiding, ghosting or highlighting any of the ~2,200
// parts is a texture write, not a scene-graph change:
//   uState  RGBA8: r = visible, g = ghost, b = highlight (128 hover, 255 selected)
//   uColor  RGBA8: sRGB surface colour
// Each system is drawn by several materials sharing one geometry: an opaque
// pass, a ghost (x-ray) pass, two stencil passes for clipping caps, and a
// GPU-picking pass that writes part ids into a 1×1 render target.

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import type { GeometryMeta, Part, PartMode } from './types';

export type ClipAxis = 'x' | 'y' | 'z';
type Pass = 'opaque' | 'ghost' | 'pick' | 'stencil' | 'detail';

/** One loaded geometry (a system at some LOD, or the detail part). */
interface Loaded {
  key: string;
  lod: number;
  geom: THREE.BufferGeometry;
  objects: THREE.Object3D[];      // in the main scene (incl. stencil)
  stencil: THREE.Mesh[];          // clipping-cap stencil passes (visible only while clipping)
  pick: THREE.Mesh;               // in the pick scene
  ranges: Map<number, [number, number]>;   // part → [first index, index count]
  triangles: number;
}

/** Highlight colour: the page's --accent token, so the 3D view follows the UI theme. */
function accentColor(): THREE.Color {
  const css = getComputedStyle(document.documentElement).getPropertyValue('--accent').trim();
  return new THREE.Color(css || '#2dd4bf');
}

export class AtlasView {
  readonly renderer: THREE.WebGLRenderer;
  readonly camera = new THREE.PerspectiveCamera(30, 1, 1, 2000);
  private readonly scene = new THREE.Scene();
  private readonly pickScene = new THREE.Scene();
  private readonly controls: OrbitControls;
  private readonly pickTarget = new THREE.WebGLRenderTarget(1, 1);

  private parts: Part[] = [];
  private size = 1;
  private state = new Uint8Array(4);
  private colors = new Uint8Array(4);
  private readonly uniforms = {
    uState: { value: null as THREE.DataTexture | null },
    uColor: { value: null as THREE.DataTexture | null },
    uTexSize: { value: 1 },
    uDetail: { value: -1 },
    uAccent: { value: accentColor() },
  };
  private loaded = new Map<string, Loaded>();
  private readonly clipPlane = new THREE.Plane(new THREE.Vector3(-1, 0, 0), 0);
  private clipOn = false;
  private readonly cap: THREE.Mesh;
  private goal: { pos: THREE.Vector3; target: THREE.Vector3 } | null = null;
  private frames = 0;
  private fpsT = performance.now();
  /** Frames per second over the last second. */
  fps = 0;
  /** Called every frame after rendering (labels follow the camera). */
  onFrame: (() => void) | null = null;

  constructor(private readonly host: HTMLElement) {
    this.renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true, stencil: true });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    this.renderer.toneMappingExposure = 1.1;
    this.renderer.localClippingEnabled = true;
    host.prepend(this.renderer.domElement);

    this.controls = new OrbitControls(this.camera, this.renderer.domElement);
    this.controls.enableDamping = true;
    this.controls.dampingFactor = 0.09;
    this.controls.minDistance = 5;
    this.controls.maxDistance = 900;
    this.controls.addEventListener('start', () => (this.goal = null));

    this.scene.add(new THREE.HemisphereLight(0xf1fbfa, 0x1d3436, 1.3));
    const key = new THREE.DirectionalLight(0xffffff, 2.0);
    key.position.set(120, 260, 300);
    const fill = new THREE.DirectionalLight(0xc8ecff, 0.7);
    fill.position.set(-250, 80, 120);
    const rim = new THREE.DirectionalLight(0xffffff, 1.0);
    rim.position.set(0, 150, -300);
    // Lights follow the camera so the side you look at is always lit.
    const rig = new THREE.Group();
    rig.add(key, fill, rim);
    this.camera.add(rig);
    this.scene.add(this.camera);

    this.cap = new THREE.Mesh(
      new THREE.PlaneGeometry(1000, 1000),
      new THREE.MeshStandardMaterial({
        color: 0xd9a093, roughness: 0.85, side: THREE.DoubleSide,
        stencilWrite: true, stencilRef: 0, stencilFunc: THREE.NotEqualStencilFunc,
        stencilFail: THREE.ReplaceStencilOp, stencilZFail: THREE.ReplaceStencilOp, stencilZPass: THREE.ReplaceStencilOp,
      }),
    );
    this.cap.renderOrder = 2;
    this.cap.visible = false;
    this.cap.onAfterRender = (r) => r.clearStencil();
    this.scene.add(this.cap);

    new ResizeObserver(() => this.resize()).observe(host);
    this.resize();
    this.renderer.setAnimationLoop(() => this.tick());
  }

  // ── Parts & state ──────────────────────────────────────────────────────────

  /** Register the part table and their colours. All parts start hidden. */
  setParts(parts: Part[], color: (p: Part) => [number, number, number]) {
    this.parts = parts;
    this.size = Math.max(1, Math.ceil(Math.sqrt(parts.length)));
    this.state = new Uint8Array(this.size * this.size * 4);
    this.colors = new Uint8Array(this.size * this.size * 4);
    for (const p of parts) this.colors.set([...color(p), 255], p.index * 4);
    const tex = (data: Uint8Array) => {
      const t = new THREE.DataTexture(data, this.size, this.size, THREE.RGBAFormat, THREE.UnsignedByteType);
      t.magFilter = t.minFilter = THREE.NearestFilter;
      t.needsUpdate = true;
      return t;
    };
    this.uniforms.uState.value = tex(this.state);
    this.uniforms.uColor.value = tex(this.colors);
    this.uniforms.uTexSize.value = this.size;
  }

  /** Set one part's visibility mode. */
  setMode(index: number, mode: PartMode) {
    this.state[index * 4] = mode === 'hidden' ? 0 : 255;
    this.state[index * 4 + 1] = mode === 'ghost' ? 255 : 0;
    this.touch();
  }

  /** Current visibility mode of a part. */
  mode(index: number): PartMode {
    if (!this.state[index * 4]) return 'hidden';
    return this.state[index * 4 + 1] ? 'ghost' : 'visible';
  }

  /** Highlight: 0 none, 1 hover, 2 selected. */
  setHighlight(index: number, level: 0 | 1 | 2) {
    if (index < 0) return;
    this.state[index * 4 + 2] = level === 2 ? 255 : level === 1 ? 128 : 0;
    this.touch();
  }

  private touch() {
    if (this.uniforms.uState.value) this.uniforms.uState.value.needsUpdate = true;
  }

  // ── Geometry ───────────────────────────────────────────────────────────────

  /**
   * Show geometry read from a SharedBuffer.  Replaces whatever was loaded
   * under the same key (e.g. a coarser LOD of the same system).
   * @param key   System name, or "detail" for the focused part.
   * @param lod   LOD index the geometry was built at.
   * @param meta  Layout from the backend.
   * @param bytes Buffer contents (copied out of the SharedBuffer).
   */
  setGeometry(key: string, lod: number, meta: GeometryMeta, bytes: ArrayBuffer) {
    const L = meta.layout;
    const geom = new THREE.BufferGeometry();
    geom.setAttribute('position', new THREE.BufferAttribute(new Float32Array(bytes, L.positions, meta.vertices * 3), 3));
    geom.setAttribute('normal', new THREE.BufferAttribute(new Float32Array(bytes, L.normals, meta.vertices * 3), 3));
    const part = new Uint16Array(bytes, L.part, meta.vertices);
    geom.setAttribute('partIdx', new THREE.BufferAttribute(part, 1));
    const index = new Uint32Array(bytes, L.indices, meta.triangles * 3);
    geom.setIndex(new THREE.BufferAttribute(index, 1));
    geom.computeBoundingSphere();

    // Parts were appended one after another: record each part's index range.
    const ranges = new Map<number, [number, number]>();
    for (let t = 0; t < index.length; t += 3) {
      const p = part[index[t]];
      const r = ranges.get(p);
      if (r) r[1] = t + 3 - r[0];
      else ranges.set(p, [t, 3]);
    }

    const detail = key === 'detail';
    const opaque = new THREE.Mesh(geom, this.material(detail ? 'detail' : 'opaque'));
    const ghost = new THREE.Mesh(geom, this.material('ghost'));
    const back = new THREE.Mesh(geom, this.material('stencil', THREE.BackSide));
    const front = new THREE.Mesh(geom, this.material('stencil', THREE.FrontSide));
    ghost.renderOrder = 3;
    back.renderOrder = front.renderOrder = 1;
    for (const m of [opaque, ghost, back, front]) m.frustumCulled = false;
    back.visible = front.visible = this.clipOn;
    const pick = new THREE.Mesh(geom, this.material('pick'));
    pick.frustumCulled = false;

    this.removeGeometry(key);
    const objects: THREE.Object3D[] = detail ? [opaque, back, front] : [opaque, ghost, back, front];
    this.scene.add(...objects);
    this.pickScene.add(pick);
    this.loaded.set(key, { key, lod, geom, objects, stencil: [back, front], pick, ranges, triangles: meta.triangles });
  }

  /** Drop a loaded geometry. */
  removeGeometry(key: string) {
    const old = this.loaded.get(key);
    if (!old) return;
    for (const o of old.objects) this.scene.remove(o);
    this.pickScene.remove(old.pick);
    old.geom.dispose();
    this.loaded.delete(key);
  }

  /** Which LOD a key is loaded at, or undefined. */
  loadedLod(key: string): number | undefined {
    return this.loaded.get(key)?.lod;
  }

  /** Render the full-detail mesh for @p index instead of its system's copy (-1 = none). */
  setDetailPart(index: number) {
    this.uniforms.uDetail.value = index;
    if (index < 0) this.removeGeometry('detail');
  }

  /** Triangles currently resident on the GPU. */
  get triangles(): number {
    let n = 0;
    for (const l of this.loaded.values()) n += l.triangles;
    return n;
  }

  private material(pass: Pass, side: THREE.Side = THREE.DoubleSide): THREE.Material {
    let mat: THREE.Material;
    if (pass === 'opaque' || pass === 'detail') {
      mat = new THREE.MeshStandardMaterial({ roughness: 0.62, metalness: 0.0, side: THREE.DoubleSide });
    } else if (pass === 'ghost') {
      mat = new THREE.MeshStandardMaterial({
        roughness: 0.5, transparent: true, opacity: 0.085, depthWrite: false, side: THREE.DoubleSide,
      });
    } else if (pass === 'pick') {
      mat = new THREE.MeshBasicMaterial({ side: THREE.DoubleSide });
    } else {
      mat = new THREE.MeshBasicMaterial({
        side, colorWrite: false, depthWrite: false, depthTest: false, stencilWrite: true,
        stencilFunc: THREE.AlwaysStencilFunc,
        stencilFail: side === THREE.BackSide ? THREE.IncrementWrapStencilOp : THREE.DecrementWrapStencilOp,
        stencilZFail: side === THREE.BackSide ? THREE.IncrementWrapStencilOp : THREE.DecrementWrapStencilOp,
        stencilZPass: side === THREE.BackSide ? THREE.IncrementWrapStencilOp : THREE.DecrementWrapStencilOp,
      });
    }
    mat.clippingPlanes = this.clipOn ? [this.clipPlane] : null;

    // Which parts this pass draws (others collapse to a degenerate point).
    const show = {
      opaque: 'st.r > 0.5 && st.g < 0.5 && abs(partIdx - uDetail) > 0.5',
      stencil: 'st.r > 0.5 && st.g < 0.5',
      pick: 'st.r > 0.5 && st.g < 0.5',
      ghost: 'st.r > 0.5 && st.g > 0.5',
      detail: 'st.r > 0.5 && st.g < 0.5 && abs(partIdx - uDetail) < 0.5',
    }[pass];
    const u = this.uniforms;
    mat.onBeforeCompile = (sh) => {
      Object.assign(sh.uniforms, u);
      sh.vertexShader = sh.vertexShader
        .replace('#include <common>', `#include <common>
          attribute float partIdx;
          uniform sampler2D uState; uniform sampler2D uColor; uniform float uTexSize; uniform float uDetail;
          varying vec3 vPartColor; varying float vHi; varying float vPart;`)
        .replace('#include <fog_vertex>', `#include <fog_vertex>
          ivec2 tc = ivec2(int(mod(partIdx, uTexSize)), int(floor(partIdx / uTexSize)));
          vec4 st = texelFetch(uState, tc, 0);
          vPartColor = pow(texelFetch(uColor, tc, 0).rgb, vec3(2.2));
          vHi = st.b; vPart = partIdx;
          if (!(${show})) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);`);
      sh.fragmentShader = sh.fragmentShader.replace('#include <common>', `#include <common>
          uniform vec3 uAccent; varying vec3 vPartColor; varying float vHi; varying float vPart;`);
      if (pass === 'pick') {
        sh.fragmentShader = sh.fragmentShader.replace('#include <color_fragment>', `#include <color_fragment>
          float pid = vPart + 1.0;
          diffuseColor = vec4(mod(pid, 256.0) / 255.0, floor(pid / 256.0) / 255.0, 0.0, 1.0);`);
      } else if (pass !== 'stencil') {
        sh.fragmentShader = sh.fragmentShader
          .replace('#include <color_fragment>', '#include <color_fragment>\ndiffuseColor.rgb = vPartColor;')
          .replace('#include <emissivemap_fragment>', `#include <emissivemap_fragment>
            totalEmissiveRadiance += uAccent * (vHi > 0.75 ? 0.5 : (vHi > 0.25 ? 0.2 : 0.0));`);
      }
    };
    mat.customProgramCacheKey = () => `atlas-${pass}-${side}`;
    return mat;
  }

  // ── Clipping ───────────────────────────────────────────────────────────────

  /**
   * Cut the body with an axis-aligned plane; solid caps fill the cut.
   * @param on     Enable/disable.
   * @param axis   x = sagittal, y = transverse, z = coronal.
   * @param value  Plane position (cm, scene frame).
   * @param flip   Keep the other side.
   */
  setClip(on: boolean, axis: ClipAxis, value: number, flip: boolean) {
    const n = new THREE.Vector3(axis === 'x' ? -1 : 0, axis === 'y' ? -1 : 0, axis === 'z' ? -1 : 0);
    if (flip) n.negate();
    this.clipPlane.set(n, flip ? -value : value);
    if (on !== this.clipOn) {
      this.clipOn = on;
      const visit = (o: THREE.Object3D) => {
        const m = (o as THREE.Mesh).material as THREE.Material | undefined;
        if (!m || o === this.cap) return;
        m.clippingPlanes = on ? [this.clipPlane] : null;
        m.needsUpdate = true;
      };
      this.scene.traverse(visit);
      this.pickScene.traverse(visit);
      for (const l of this.loaded.values()) for (const m of l.stencil) m.visible = on;
    }
    this.cap.visible = on;
    this.clipPlane.coplanarPoint(this.cap.position);
    this.cap.lookAt(this.cap.position.clone().add(this.clipPlane.normal));
  }

  // ── Picking ────────────────────────────────────────────────────────────────

  /**
   * Part under a viewport pixel (GPU id buffer), or -1.
   * @param x, y  CSS pixels relative to the viewport.
   */
  pick(x: number, y: number): number {
    const w = this.host.clientWidth, h = this.host.clientHeight;
    if (x < 0 || y < 0 || x >= w || y >= h) return -1;
    const r = this.renderer;
    const prevTarget = r.getRenderTarget();
    const prevClear = r.getClearColor(new THREE.Color());
    const prevAlpha = r.getClearAlpha();
    this.camera.setViewOffset(w, h, Math.floor(x), Math.floor(y), 1, 1);
    r.setRenderTarget(this.pickTarget);
    r.setClearColor(0x000000, 0);
    r.clear();
    r.render(this.pickScene, this.camera);
    const px = new Uint8Array(4);
    r.readRenderTargetPixels(this.pickTarget, 0, 0, 1, 1, px);
    this.camera.clearViewOffset();
    r.setRenderTarget(prevTarget);
    r.setClearColor(prevClear, prevAlpha);
    const id = px[0] + px[1] * 256 - 1;
    return id >= 0 && id < this.parts.length ? id : -1;
  }

  /**
   * Point where the ray through a viewport pixel first hits part @p index.
   * Used to anchor labels on the surface.
   */
  surfacePoint(index: number, x: number, y: number): THREE.Vector3 | null {
    const w = this.host.clientWidth, h = this.host.clientHeight;
    const ray = new THREE.Raycaster();
    ray.setFromCamera(new THREE.Vector2((x / w) * 2 - 1, -(y / h) * 2 + 1), this.camera);
    let best: THREE.Vector3 | null = null;
    let bestD = Infinity;
    const a = new THREE.Vector3(), b = new THREE.Vector3(), c = new THREE.Vector3(), hit = new THREE.Vector3();
    const order = this.uniforms.uDetail.value === index ? ['detail', ...this.loaded.keys()] : [...this.loaded.keys()];
    for (const key of order) {
      const l = this.loaded.get(key);
      const r = l?.ranges.get(index);
      if (!l || !r) continue;
      const pos = l.geom.attributes.position as THREE.BufferAttribute;
      const idx = l.geom.index!.array;
      for (let t = r[0]; t < r[0] + r[1]; t += 3) {
        a.fromBufferAttribute(pos, idx[t]);
        b.fromBufferAttribute(pos, idx[t + 1]);
        c.fromBufferAttribute(pos, idx[t + 2]);
        if (ray.ray.intersectTriangle(a, b, c, false, hit)) {
          const d = hit.distanceTo(ray.ray.origin);
          if (d < bestD) { bestD = d; best = hit.clone(); }
        }
      }
      if (best) break;
    }
    return best;
  }

  /** Viewport pixel of a world point (null when behind the camera). */
  project(p: THREE.Vector3): { x: number; y: number } | null {
    const v = p.clone().project(this.camera);
    if (v.z > 1 || v.z < -1) return null;
    return { x: ((v.x + 1) / 2) * this.host.clientWidth, y: ((1 - v.y) / 2) * this.host.clientHeight };
  }

  // ── Camera ─────────────────────────────────────────────────────────────────

  /**
   * Frame an axis-aligned box, keeping the current viewing direction
   * unless @p from is given ("front" looks at the body's anterior side).
   */
  frame(min: THREE.Vector3, max: THREE.Vector3, from?: 'front' | 'keep' | THREE.Vector3, instant = false) {
    const center = min.clone().add(max).multiplyScalar(0.5);
    const radius = Math.max(4, max.clone().sub(min).length() / 2);
    const fov = THREE.MathUtils.degToRad(this.camera.fov);
    const fit = Math.min(fov, 2 * Math.atan(Math.tan(fov / 2) * this.camera.aspect));
    const dist = (radius / Math.sin(fit / 2)) * 0.92;
    const dir = from instanceof THREE.Vector3
      ? from.clone().normalize()
      : from === 'front' || !this.loaded.size
        ? new THREE.Vector3(0.18, 0.12, 1).normalize()
        : this.camera.position.clone().sub(this.controls.target).normalize();
    const pos = center.clone().addScaledVector(dir, dist);
    if (instant) {
      this.camera.position.copy(pos);
      this.controls.target.copy(center);
      this.controls.update();
      this.goal = null;
    } else {
      this.goal = { pos, target: center };
    }
  }

  private resize() {
    const w = Math.max(1, this.host.clientWidth), h = Math.max(1, this.host.clientHeight);
    this.renderer.setSize(w, h, false);
    this.renderer.domElement.style.width = `${w}px`;
    this.renderer.domElement.style.height = `${h}px`;
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
  }

  private countFrame() {
    this.frames++;
    const now = performance.now();
    if (now - this.fpsT >= 1000) {
      this.fps = Math.round((this.frames * 1000) / (now - this.fpsT));
      this.frames = 0;
      this.fpsT = now;
    }
  }

  private tick() {
    if (this.goal) {
      this.camera.position.lerp(this.goal.pos, 0.12);
      this.controls.target.lerp(this.goal.target, 0.12);
      if (this.camera.position.distanceTo(this.goal.pos) < 0.05) this.goal = null;
    }
    this.controls.update();
    this.renderer.render(this.scene, this.camera);
    this.countFrame();
    this.onFrame?.();
  }
}
