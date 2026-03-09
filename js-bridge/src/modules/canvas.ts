// ---------------------------------------------------------------------------
// @libanyar/api/canvas — WebGL Frame Renderer
//
// Provides a high-performance rendering pipeline for displaying raw pixel
// data (RGBA or YUV420) from shared memory buffers on a <canvas> element
// using WebGL.
//
// This module consumes @libanyar/api/buffer internally and provides a
// simple event-driven API for streaming use cases (video, LiDAR, etc.).
// ---------------------------------------------------------------------------

import { fetchBuffer, onBufferReady, poolReleaseRead } from './buffer';
import type { BufferReadyEvent, SharedBufferPoolInfo } from './buffer';
import type { UnlistenFn } from '../types';

// Re-export buffer types for convenience
export type { BufferReadyEvent, SharedBufferPoolInfo };

// ── Types ──────────────────────────────────────────────────────────────────

/** Pixel format of the incoming data. */
export type PixelFormat = 'rgba' | 'rgb' | 'bgra' | 'grayscale' | 'yuv420' | 'nv12' | 'nv21';

/** Configuration for the frame renderer. */
export interface FrameRendererOptions {
  /** Target canvas element or CSS selector string. */
  canvas: HTMLCanvasElement | string;
  /** Width of each frame in pixels. */
  width: number;
  /** Height of each frame in pixels. */
  height: number;
  /** Pixel format. Default: 'rgba'. */
  format?: PixelFormat;
}

/** Options for the pool-backed buffer renderer. */
export interface BufferRendererOptions extends FrameRendererOptions {
  /** Pool base name to listen for buffer:ready events. */
  pool: string;
  /** Whether to automatically release the buffer back to the pool after
   *  rendering each frame. Default: true. */
  autoRelease?: boolean;
  /** Optional callback invoked after each frame is drawn. */
  onFrame?: (event: BufferReadyEvent) => void;
}

// ── Shader sources ─────────────────────────────────────────────────────────

const VERTEX_SHADER_SRC = `
  attribute vec2 a_position;
  attribute vec2 a_texCoord;
  varying vec2 v_texCoord;
  void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_texCoord = a_texCoord;
  }
`;

const FRAGMENT_SHADER_RGBA = `
  precision mediump float;
  varying vec2 v_texCoord;
  uniform sampler2D u_texture;
  void main() {
    gl_FragColor = texture2D(u_texture, v_texCoord);
  }
`;

const FRAGMENT_SHADER_YUV420 = `
  precision mediump float;
  varying vec2 v_texCoord;
  uniform sampler2D u_textureY;
  uniform sampler2D u_textureU;
  uniform sampler2D u_textureV;
  void main() {
    float y = texture2D(u_textureY, v_texCoord).r;
    float u = texture2D(u_textureU, v_texCoord).r - 0.5;
    float v = texture2D(u_textureV, v_texCoord).r - 0.5;
    // BT.601 conversion
    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;
    gl_FragColor = vec4(clamp(r, 0.0, 1.0),
                        clamp(g, 0.0, 1.0),
                        clamp(b, 0.0, 1.0),
                        1.0);
  }
`;

const FRAGMENT_SHADER_NV12 = `
  precision mediump float;
  varying vec2 v_texCoord;
  uniform sampler2D u_textureY;
  uniform sampler2D u_textureUV;
  void main() {
    float y = texture2D(u_textureY, v_texCoord).r;
    vec2 uv = texture2D(u_textureUV, v_texCoord).ra;
    float u = uv.x - 0.5;
    float v = uv.y - 0.5;
    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;
    gl_FragColor = vec4(clamp(r, 0.0, 1.0),
                        clamp(g, 0.0, 1.0),
                        clamp(b, 0.0, 1.0),
                        1.0);
  }
`;

const FRAGMENT_SHADER_NV21 = `
  precision mediump float;
  varying vec2 v_texCoord;
  uniform sampler2D u_textureY;
  uniform sampler2D u_textureUV;
  void main() {
    float y = texture2D(u_textureY, v_texCoord).r;
    vec2 uv = texture2D(u_textureUV, v_texCoord).ar;
    float u = uv.x - 0.5;
    float v = uv.y - 0.5;
    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;
    gl_FragColor = vec4(clamp(r, 0.0, 1.0),
                        clamp(g, 0.0, 1.0),
                        clamp(b, 0.0, 1.0),
                        1.0);
  }
`;

const FRAGMENT_SHADER_RGB = `
  precision mediump float;
  varying vec2 v_texCoord;
  uniform sampler2D u_texture;
  void main() {
    gl_FragColor = vec4(texture2D(u_texture, v_texCoord).rgb, 1.0);
  }
`;

const FRAGMENT_SHADER_BGRA = `
  precision mediump float;
  varying vec2 v_texCoord;
  uniform sampler2D u_texture;
  void main() {
    vec4 c = texture2D(u_texture, v_texCoord);
    gl_FragColor = vec4(c.b, c.g, c.r, c.a);
  }
`;

const FRAGMENT_SHADER_GRAYSCALE = `
  precision mediump float;
  varying vec2 v_texCoord;
  uniform sampler2D u_texture;
  void main() {
    float l = texture2D(u_texture, v_texCoord).r;
    gl_FragColor = vec4(l, l, l, 1.0);
  }
`;

/** Map each pixel format to its fragment shader source. */
const FRAGMENT_SHADERS: Record<PixelFormat, string> = {
  rgba: FRAGMENT_SHADER_RGBA,
  rgb: FRAGMENT_SHADER_RGB,
  bgra: FRAGMENT_SHADER_BGRA,
  grayscale: FRAGMENT_SHADER_GRAYSCALE,
  yuv420: FRAGMENT_SHADER_YUV420,
  nv12: FRAGMENT_SHADER_NV12,
  nv21: FRAGMENT_SHADER_NV21,
};

// Fullscreen quad: two triangles covering clip space [-1, 1]
const QUAD_POSITIONS = new Float32Array([
  -1, -1,   1, -1,  -1,  1,
  -1,  1,   1, -1,   1,  1,
]);

// Texture coordinates: flip Y so top-left = (0, 0)
const QUAD_TEXCOORDS = new Float32Array([
  0, 1,  1, 1,  0, 0,
  0, 0,  1, 1,  1, 0,
]);

// ── FrameRenderer class ────────────────────────────────────────────────────

/**
 * WebGL-based frame renderer that efficiently displays raw pixel data
 * (RGBA or YUV420) on a canvas element.
 *
 * Usage:
 * ```ts
 * const renderer = createFrameRenderer({
 *   canvas: '#my-canvas',
 *   width: 1920,
 *   height: 1080,
 *   format: 'rgba',
 * });
 * // Draw a frame from an ArrayBuffer
 * renderer.drawFrame(pixelData);
 * // Clean up
 * renderer.destroy();
 * ```
 */
export class FrameRenderer {
  private gl: WebGLRenderingContext;
  private program: WebGLProgram;
  private format: PixelFormat;
  private width: number;
  private height: number;

  // Single-texture formats: RGBA, RGB, BGRA, Grayscale
  private textureRGBA: WebGLTexture | null = null;

  // YUV420: three separate plane textures (Y, U, V)
  private textureY: WebGLTexture | null = null;
  private textureU: WebGLTexture | null = null;
  private textureV: WebGLTexture | null = null;

  // NV12/NV21: Y plane + interleaved UV plane
  private textureUV: WebGLTexture | null = null;

  private destroyed = false;

  constructor(options: FrameRendererOptions) {
    const canvas = typeof options.canvas === 'string'
      ? document.querySelector<HTMLCanvasElement>(options.canvas)
      : options.canvas;

    if (!canvas) {
      throw new Error(`Canvas element not found: ${options.canvas}`);
    }

    this.width = options.width;
    this.height = options.height;
    this.format = options.format ?? 'rgba';

    // Set canvas dimensions
    canvas.width = this.width;
    canvas.height = this.height;

    const gl = canvas.getContext('webgl', {
      alpha: false,
      antialias: false,
      premultipliedAlpha: false,
      preserveDrawingBuffer: false,
    });

    if (!gl) {
      throw new Error('WebGL not available');
    }

    this.gl = gl;

    // Ensure correct row alignment for all pixel formats (RGB, grayscale, etc.)
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);

    // Create shader program (format → fragment shader lookup)
    const fragSrc = FRAGMENT_SHADERS[this.format];

    this.program = this.createProgram(VERTEX_SHADER_SRC, fragSrc);
    gl.useProgram(this.program);

    // Set up geometry
    this.setupGeometry();

    // Create textures based on format
    switch (this.format) {
      case 'yuv420':
        this.textureY = this.createTexture(this.width, this.height);
        this.textureU = this.createTexture(this.width / 2, this.height / 2);
        this.textureV = this.createTexture(this.width / 2, this.height / 2);
        break;
      case 'nv12':
      case 'nv21':
        this.textureY = this.createTexture(this.width, this.height);
        this.textureUV = this.createTexture(this.width / 2, this.height / 2);
        break;
      default: // rgba, rgb, bgra, grayscale
        this.textureRGBA = this.createTexture(this.width, this.height);
        break;
    }

    // Set viewport
    gl.viewport(0, 0, this.width, this.height);
  }

  /**
   * Draw a frame from raw pixel data.
   *
   * @param data Raw pixel data (buffer sizes per format):
   *   - `rgba`:      width × height × 4 bytes
   *   - `rgb`:       width × height × 3 bytes
   *   - `bgra`:      width × height × 4 bytes
   *   - `grayscale`: width × height bytes
   *   - `yuv420`:    width × height × 1.5 bytes (Y + U + V planes)
   *   - `nv12`:      width × height × 1.5 bytes (Y plane + interleaved UV)
   *   - `nv21`:      width × height × 1.5 bytes (Y plane + interleaved VU)
   */
  drawFrame(data: ArrayBuffer | Uint8Array): void {
    if (this.destroyed) return;

    const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
    const gl = this.gl;

    gl.useProgram(this.program);

    switch (this.format) {
      case 'yuv420':
        this.drawYUV420(gl, bytes);
        break;
      case 'nv12':
      case 'nv21':
        this.drawNV12(gl, bytes);
        break;
      case 'rgb':
        this.drawSingleTexture(gl, bytes, gl.RGB);
        break;
      case 'grayscale':
        this.drawSingleTexture(gl, bytes, gl.LUMINANCE);
        break;
      default: // rgba, bgra (BGRA uploaded as RGBA; shader swaps channels)
        this.drawSingleTexture(gl, bytes, gl.RGBA);
        break;
    }

    gl.drawArrays(gl.TRIANGLES, 0, 6);
  }

  /**
   * Update the frame dimensions. Creates new textures.
   *
   * @param width  New width in pixels.
   * @param height New height in pixels.
   */
  resize(width: number, height: number): void {
    if (this.destroyed) return;

    this.width = width;
    this.height = height;

    const gl = this.gl;
    const canvas = gl.canvas as HTMLCanvasElement;
    canvas.width = width;
    canvas.height = height;
    gl.viewport(0, 0, width, height);

    // Recreate textures with new dimensions
    switch (this.format) {
      case 'yuv420':
        if (this.textureY) gl.deleteTexture(this.textureY);
        if (this.textureU) gl.deleteTexture(this.textureU);
        if (this.textureV) gl.deleteTexture(this.textureV);
        this.textureY = this.createTexture(width, height);
        this.textureU = this.createTexture(width / 2, height / 2);
        this.textureV = this.createTexture(width / 2, height / 2);
        break;
      case 'nv12':
      case 'nv21':
        if (this.textureY) gl.deleteTexture(this.textureY);
        if (this.textureUV) gl.deleteTexture(this.textureUV);
        this.textureY = this.createTexture(width, height);
        this.textureUV = this.createTexture(width / 2, height / 2);
        break;
      default: // rgba, rgb, bgra, grayscale
        if (this.textureRGBA) gl.deleteTexture(this.textureRGBA);
        this.textureRGBA = this.createTexture(width, height);
        break;
    }
  }

  /** Clean up WebGL resources. */
  destroy(): void {
    if (this.destroyed) return;
    this.destroyed = true;

    const gl = this.gl;
    if (this.textureRGBA) gl.deleteTexture(this.textureRGBA);
    if (this.textureY) gl.deleteTexture(this.textureY);
    if (this.textureU) gl.deleteTexture(this.textureU);
    if (this.textureV) gl.deleteTexture(this.textureV);
    if (this.textureUV) gl.deleteTexture(this.textureUV);
    gl.deleteProgram(this.program);
  }

  // ── Private helpers ────────────────────────────────────────────────────

  private drawSingleTexture(gl: WebGLRenderingContext, bytes: Uint8Array, glFormat: number): void {
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.textureRGBA);
    gl.texImage2D(
      gl.TEXTURE_2D, 0, glFormat,
      this.width, this.height, 0,
      glFormat, gl.UNSIGNED_BYTE, bytes,
    );
    gl.uniform1i(gl.getUniformLocation(this.program, 'u_texture'), 0);
  }

  private drawYUV420(gl: WebGLRenderingContext, bytes: Uint8Array): void {
    const w = this.width;
    const h = this.height;
    const ySize = w * h;
    const uvSize = (w / 2) * (h / 2);

    const yPlane = bytes.subarray(0, ySize);
    const uPlane = bytes.subarray(ySize, ySize + uvSize);
    const vPlane = bytes.subarray(ySize + uvSize, ySize + uvSize * 2);

    // Y texture
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.textureY);
    gl.texImage2D(
      gl.TEXTURE_2D, 0, gl.LUMINANCE,
      w, h, 0,
      gl.LUMINANCE, gl.UNSIGNED_BYTE, yPlane,
    );
    gl.uniform1i(gl.getUniformLocation(this.program, 'u_textureY'), 0);

    // U texture
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.textureU);
    gl.texImage2D(
      gl.TEXTURE_2D, 0, gl.LUMINANCE,
      w / 2, h / 2, 0,
      gl.LUMINANCE, gl.UNSIGNED_BYTE, uPlane,
    );
    gl.uniform1i(gl.getUniformLocation(this.program, 'u_textureU'), 1);

    // V texture
    gl.activeTexture(gl.TEXTURE2);
    gl.bindTexture(gl.TEXTURE_2D, this.textureV);
    gl.texImage2D(
      gl.TEXTURE_2D, 0, gl.LUMINANCE,
      w / 2, h / 2, 0,
      gl.LUMINANCE, gl.UNSIGNED_BYTE, vPlane,
    );
    gl.uniform1i(gl.getUniformLocation(this.program, 'u_textureV'), 2);
  }

  private drawNV12(gl: WebGLRenderingContext, bytes: Uint8Array): void {
    const w = this.width;
    const h = this.height;
    const ySize = w * h;
    const uvSize = (w / 2) * (h / 2) * 2;

    const yPlane = bytes.subarray(0, ySize);
    const uvPlane = bytes.subarray(ySize, ySize + uvSize);

    // Y plane (LUMINANCE, full resolution)
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.textureY);
    gl.texImage2D(
      gl.TEXTURE_2D, 0, gl.LUMINANCE,
      w, h, 0,
      gl.LUMINANCE, gl.UNSIGNED_BYTE, yPlane,
    );
    gl.uniform1i(gl.getUniformLocation(this.program, 'u_textureY'), 0);

    // UV interleaved plane (LUMINANCE_ALPHA, half resolution)
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.textureUV);
    gl.texImage2D(
      gl.TEXTURE_2D, 0, gl.LUMINANCE_ALPHA,
      w / 2, h / 2, 0,
      gl.LUMINANCE_ALPHA, gl.UNSIGNED_BYTE, uvPlane,
    );
    gl.uniform1i(gl.getUniformLocation(this.program, 'u_textureUV'), 1);
  }

  private createProgram(vSrc: string, fSrc: string): WebGLProgram {
    const gl = this.gl;
    const vs = this.compileShader(gl.VERTEX_SHADER, vSrc);
    const fs = this.compileShader(gl.FRAGMENT_SHADER, fSrc);

    const program = gl.createProgram()!;
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);

    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      const info = gl.getProgramInfoLog(program);
      gl.deleteProgram(program);
      throw new Error(`Shader link failed: ${info}`);
    }

    gl.deleteShader(vs);
    gl.deleteShader(fs);
    return program;
  }

  private compileShader(type: number, source: string): WebGLShader {
    const gl = this.gl;
    const shader = gl.createShader(type)!;
    gl.shaderSource(shader, source);
    gl.compileShader(shader);

    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      const info = gl.getShaderInfoLog(shader);
      gl.deleteShader(shader);
      throw new Error(`Shader compile failed: ${info}`);
    }

    return shader;
  }

  private setupGeometry(): void {
    const gl = this.gl;
    const program = this.program;

    // Position buffer
    const posBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, QUAD_POSITIONS, gl.STATIC_DRAW);

    const posLoc = gl.getAttribLocation(program, 'a_position');
    gl.enableVertexAttribArray(posLoc);
    gl.vertexAttribPointer(posLoc, 2, gl.FLOAT, false, 0, 0);

    // Texcoord buffer
    const texBuffer = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, texBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, QUAD_TEXCOORDS, gl.STATIC_DRAW);

    const texLoc = gl.getAttribLocation(program, 'a_texCoord');
    gl.enableVertexAttribArray(texLoc);
    gl.vertexAttribPointer(texLoc, 2, gl.FLOAT, false, 0, 0);
  }

  private createTexture(_width: number, _height: number): WebGLTexture {
    const gl = this.gl;
    const tex = gl.createTexture()!;
    gl.bindTexture(gl.TEXTURE_2D, tex);

    // Set texture parameters for non-power-of-two textures
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);

    return tex;
  }
}

// ── Factory functions ──────────────────────────────────────────────────────

/**
 * Create a new WebGL frame renderer.
 *
 * @param options Renderer configuration.
 * @returns       A new FrameRenderer instance.
 */
export function createFrameRenderer(
  options: FrameRendererOptions,
): FrameRenderer {
  return new FrameRenderer(options);
}

/**
 * Create a buffer-backed renderer that automatically fetches frames from
 * shared memory and renders them to a canvas when `buffer:ready` events
 * are received.
 *
 * This is the high-level convenience API for streaming use cases.
 *
 * @param options Configuration for the buffer renderer.
 * @returns Object with `destroy()` to stop rendering and clean up.
 *
 * @example
 * ```ts
 * import { createBufferRenderer } from '@libanyar/api/canvas';
 *
 * const { destroy } = createBufferRenderer({
 *   canvas: '#video-canvas',
 *   width: 1920,
 *   height: 1080,
 *   format: 'rgba',
 *   pool: 'video-frames',
 *   onFrame: (event) => console.log('Frame rendered:', event.metadata),
 * });
 *
 * // Later...
 * destroy();
 * ```
 */
export function createBufferRenderer(
  options: BufferRendererOptions,
): { destroy: () => void } {
  const renderer = createFrameRenderer(options);
  const autoRelease = options.autoRelease ?? true;

  const unlisten = onBufferReady(async (event) => {
    // Only handle events for our pool
    if (event.pool !== options.pool) return;

    try {
      // Fetch the buffer data via anyar-shm://
      const data = await fetchBuffer(event.url);

      // Check if dimensions changed via metadata
      const meta = event.metadata;
      if (
        typeof meta.width === 'number' &&
        typeof meta.height === 'number' &&
        (meta.width !== options.width || meta.height !== options.height)
      ) {
        renderer.resize(meta.width as number, meta.height as number);
      }

      // Render the frame
      renderer.drawFrame(data);

      // Notify caller
      if (options.onFrame) {
        options.onFrame(event);
      }
    } catch (err) {
      console.error('[libanyar/canvas] Error rendering frame:', err);
    } finally {
      // Release the buffer back to the pool
      if (autoRelease && event.pool) {
        poolReleaseRead(event.pool, event.name).catch((e) =>
          console.error('[libanyar/canvas] Error releasing buffer:', e),
        );
      }
    }
  });

  return {
    destroy() {
      unlisten();
      renderer.destroy();
    },
  };
}
