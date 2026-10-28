// Shapes exchanged with the C++ AtlasPlugin, plus shared frontend types.

/** Provenance + licence block of the anatomy pack. */
export interface PackSource {
  name: string;
  version: string;
  license: string;
  license_url: string;
  attribution: string;
  source_url: string;
  license_page?: string;
}

/** Result of `atlas:info`. */
export interface AtlasInfo {
  pack: 'full' | 'sample';
  path: string;
  parts: number;
  lods: number;
  lods_mm: number[];
  height: number;
  systems: string[];
  source: PackSource;
  manifest: Record<string, unknown> | null;
}

/** One anatomical part (decoded from the compact `atlas:parts` table). */
export interface Part {
  index: number;
  id: string;
  fma: string;
  name: string;
  system: string;
  kind: string;
  region: string;
  min: [number, number, number];
  max: [number, number, number];
  /** Triangles per LOD (0 = finest). */
  triangles: number[];
}

/** Result of `atlas:load` / `atlas:load_parts` (offsets into the SharedBuffer). */
export interface GeometryMeta {
  buffer: string;
  id: number;
  bytes: number;
  vertices: number;
  triangles: number;
  ms: number;
  layout: { positions: number; normals: number; part: number; indices: number };
}

/** Visibility of one part. */
export type PartMode = 'visible' | 'hidden' | 'ghost';
