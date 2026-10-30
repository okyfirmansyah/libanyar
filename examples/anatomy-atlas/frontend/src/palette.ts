// Display names and colours for body systems and part kinds.

/** Human-readable system names, in sidebar order. */
export const SYSTEM_LABEL: Record<string, string> = {
  skeletal: 'Skeletal',
  muscular: 'Muscular',
  cardiovascular: 'Cardiovascular',
  nervous: 'Nervous',
  respiratory: 'Respiratory',
  digestive: 'Digestive',
  urogenital: 'Urogenital',
  integumentary: 'Skin & hair',
  other: 'Other organs',
};

/** Systems shown when the app opens. */
export const DEFAULT_ON = new Set(['skeletal', 'cardiovascular', 'nervous', 'respiratory', 'digestive', 'urogenital', 'other']);

/** Swatch colour per system (sidebar). */
export const SYSTEM_COLOR: Record<string, string> = {
  skeletal: '#e8dcc4',
  muscular: '#b6524a',
  cardiovascular: '#d2383f',
  nervous: '#ebc64b',
  respiratory: '#8fb7c9',
  digestive: '#c98b5b',
  urogenital: '#c17a86',
  integumentary: '#e8c1a0',
  other: '#9c6ba8',
};

/** Surface colour per part kind (sRGB). */
const KIND_COLOR: Record<string, string> = {
  bone: '#e8dcc4',
  cartilage: '#bfd8e2',
  tooth: '#f2eee3',
  muscle: '#b6524a',
  tendon: '#d9c7a6',
  artery: '#d2383f',
  vein: '#3f5fbf',
  vessel: '#a0446e',
  heart: '#b5473f',
  nerve: '#ebc64b',
  brain: '#e5b7a9',
  airway: '#8fb7c9',
  organ: '#c98b5b',
  eye: '#edebe4',
  skin: '#e8c1a0',
};

/** Human-readable region names. */
export const REGION_LABEL: Record<string, string> = {
  head: 'Head & neck',
  thorax: 'Thorax',
  abdomen: 'Abdomen',
  pelvis: 'Pelvis',
  upper_limb: 'Upper limbs',
  lower_limb: 'Lower limbs',
  whole_body: 'Whole body',
};

function hash(s: string): number {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
  return h >>> 0;
}

/**
 * Colour of one part as sRGB bytes. Organs take their system's colour; every
 * part gets a small deterministic brightness jitter so neighbours separate.
 * @param system  Part system.
 * @param kind    Part kind.
 * @param id      Part id (seed for the jitter).
 * @returns       [r, g, b] in 0..255.
 */
export function partColor(system: string, kind: string, id: string): [number, number, number] {
  const hex = kind === 'organ' ? SYSTEM_COLOR[system] ?? KIND_COLOR.organ : KIND_COLOR[kind] ?? SYSTEM_COLOR[system] ?? '#c0a090';
  const n = parseInt(hex.slice(1), 16);
  const j = 0.9 + ((hash(id) % 1000) / 1000) * 0.16;
  return [((n >> 16) & 255) * j, ((n >> 8) & 255) * j, (n & 255) * j].map((c) => Math.min(255, Math.round(c))) as [
    number,
    number,
    number,
  ];
}
