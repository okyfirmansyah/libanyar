# 3D Medical Anatomy Tools — Roadmap

> **Status**: A0–A2 done on branch `body_atlas_demo` (2026-10-21); the full pack still needs its GitHub Release upload. B-track not started. Planning facts below dated 2026-10-21. The licence facts below were checked against the primary sources on 2026-10-21. Re-check them before each release, because licences change: BodyParts3D moved from CC BY-SA 2.1 JP to CC BY 4.0 without the downloaded files being updated.
> **Prior art**: the `mics_demo` branch (MICS-CABG planner) proved the pipeline. BodyParts3D meshes are packed and embedded with CMakeRC; C++ rebuilds the myocardium, extracts centerlines and hands one SharedBuffer to three.js. Reuse it rather than starting over.

## Goal

A set of generic 3D anatomy tools that show what LibAnyar does better than a pure web app: native C++ processing of large anatomy and imaging data, zero-copy handoff to WebGL, background work that never blocks the UI, multiple synced windows, and native packaging.

**Non-goals.** This is not a medical device. There is no diagnosis or treatment planning for real patients, no clinical validation and no regulatory submission. Every user-facing surface says so (see A0.3).

---

## Licensing rules (apply to every phase)

1. **The repo needs a licence first.** `main` has no `LICENSE` today, so nobody may legally reuse the code. Pick MIT or Apache-2.0 before anything is showcased.
2. **Code licence ≠ asset licence.** Third-party anatomy data lives under `assets/<source>/` with its own `LICENSE`. A root `NOTICE` file lists every third-party asset and library with its licence and the exact attribution wording.
3. **What may be committed to the repo:**

   | Licence | Commit? | Notes |
   |---|---|---|
   | CC0, public domain | ✅ | Attribution still recommended |
   | CC BY 4.0 | ✅ | Exact attribution in NOTICE, the asset README and the app's About/credits |
   | NLM Terms (Visible Human) | ⚠️ download at runtime | Keeps the repo small; show the NLM credit in the app |
   | CC BY-SA (any version) | ❌ not in the repo | Optional download into a separate folder that carries its own BY-SA licence; never mixed into code or CC BY packs |
   | Any **NC** (non-commercial) or "contact us for commercial use" | ❌ never | Includes the licensed TotalSegmentator tasks |
   | Unknown / unclear | ❌ | Until the licence is confirmed in writing |

4. **Provenance manifest.** Every asset pack ships an `assets/<source>/MANIFEST.json` listing the source URL, release or version, download date, licence identifier, licence URL, attribution text, the SHA-256 of each source archive, and the tool and command that produced the pack.
5. **Attribution in three places:** NOTICE, the asset README, and the app UI (viewport credit line plus an About dialog). Packaged builds (DEB, AppImage, MSI, NSIS) include NOTICE.
6. **A CI licence gate** (A0.4) fails the build if an asset has no manifest, a manifest names a licence outside the allow-list, or NOTICE is missing an entry.
7. **Libraries:** permissive licences only (MIT, BSD, Apache-2.0, BSL, zlib). No GPL or AGPL in the shipped binary. LGPL only after a decision recorded in an ADR.

### Sources (checked 2026-10-21)

| Source | Use for | Licence | Attribution (exact) | Policy |
|---|---|---|---|---|
| [BodyParts3D](https://dbarchive.biosciencedbc.jp/en/bodyparts3d/lic.html) | Whole-body meshes (skeleton, muscles, organs, vessels, brain), FMA-labelled | **CC BY 4.0** (page updated 2025-02-27; OBJ headers in the 4.0 archive still say CC BY-SA 2.1 JP, but the website is authoritative) | "BodyParts3D, © The Database Center for Life Science licensed under CC Attribution 4.0 International" | ✅ Primary source, committed (small packs) or as a release asset (full body) |
| [HRA 3D Reference Object Library](https://humanatlas.io/3d-reference-library) | Higher-detail organs (male and female) | CC BY 4.0 (confirmed per model on [NIH 3D](https://3d.nih.gov/entries/20960)) | Per model, from its page | ✅ Optional organ upgrade; check each model's badge |
| [Visible Human Project](https://www.nlm.nih.gov/research/visible/getting_data.html) | Sample CT/MRI volumes | [NLM Terms](https://www.nlm.nih.gov/databases/download/terms_and_conditions.html) (no licence agreement or registration since 2019) | "Courtesy of the U.S. National Library of Medicine". Do not imply NLM endorsement. Say if the data may not be the latest | ⚠️ Downloaded at runtime |
| [TotalSegmentator](https://github.com/wasserth/TotalSegmentator) | CT/MR auto-segmentation | Apache-2.0 for code and open tasks (`total`, `total_mr`, `body`, `lung_vessels`, …). Others need a licence; `brain_aneurysm` is CC BY-NC | Cite per README | ✅ Open tasks only; a runtime allow-list enforces it |
| [Z-Anatomy](https://github.com/Z-Anatomy/Models-of-human-anatomy) | Nicer meshes, extra structures | CC BY-SA 4.0 | Z-Anatomy + BodyParts3D | ❌ Not in the repo. Possible later as an opt-in download with its own BY-SA licence |
| [Open Anatomy / SPL atlases](https://openanatomy.org/) | Brain, head and neck, abdomen, knee | "3D Slicer License section B" (not yet read) | — | ❓ Blocked until the licence text is reviewed |
| TCIA collections | Extra CT/MRI samples | Per collection (CC BY / CC BY-NC mix) | Per collection | ❓ Per-collection review; never NC |

### Libraries (verify each before adding)

| Library | Purpose | Expected licence |
|---|---|---|
| three.js | Rendering | MIT |
| DCMTK or GDCM | DICOM I/O | BSD-style |
| ITK (optional) | Resampling, registration | Apache-2.0 |
| ONNX Runtime | Running segmentation models in-process | MIT |
| meshoptimizer | Mesh simplification and LOD | MIT |

Existing dependencies (Boost: BSL, nlohmann/json: MIT, LibAsyik: to be confirmed) go into NOTICE as well.

---

## Phase A0 — Legal & repo groundwork (do first, ~1 day)

- [x] **A0.1** Add a root `LICENSE` (MIT or Apache-2.0) and record the choice in an ADR in `docs/decisions.md`. *MIT, ADR-015.*
- [x] **A0.2** Add a root `NOTICE` covering existing dependencies, then each asset source as it lands.
- [x] **A0.3** Write the medical disclaimer: "For education and demonstration only. Not a medical device; not for diagnosis or treatment." Put it in the README, each example README, an app footer and the About dialog.
- [x] **A0.4** Add `scripts/check_assets.py`, which validates every `assets/**/MANIFEST.json` against the allow-list and NOTICE. Run it in CircleCI and GitHub Actions. *CircleCI `licence-check` job + a Windows workflow step; negative-tested (NC licence, uncovered data file).*
- [x] **A0.5** Fix the `mics_demo` branch's `assets/README.md`: change it to CC BY 4.0 with the new wording and drop the share-alike note. *Commit `72f9d85` on `mics_demo` (also the pack header, credits and docs).*

**Exit:** CI is green with the licence gate on, and the README shows the licence badge and disclaimer.

## Phase A1 — Anatomy asset pipeline (~1 week)

Generalise `tools/build_anatomy_pack.py` from `mics_demo`.

- [x] **A1.1** Read the **whole** BodyParts3D 4.0 release (both the `isa` and `partof` trees). Build the anatomy tree from the FMA hierarchy files: system → region → organ → part.
  *Done as: all 2,234 is-a elements; system + type from is-a/part-of ancestors with name-keyword fallbacks; region from the part's position. The UI tree is system → region → part (no organ level yet).*
- [x] **A1.2** Pack format v2 (`ANATPAK2`):
  - chunked by body system (skeleton, muscles, cardiovascular, nervous, digestive, respiratory, urogenital, …);
  - two levels of detail per part (simplified with meshoptimizer);
  - 16-bit quantised positions;
  - an index of part → FMA ID, names and bounds.
- [x] **A1.3** Write `MANIFEST.json` automatically: source URLs, archive SHA-256, the licence (CC BY 4.0) and the attribution string. Note in the manifest that the archive headers still say BY-SA 2.1 JP and the website licence page takes precedence.
- [x] **A1.4** Distribution:
  - a **thorax sample pack** (≤ 3 MB) committed to the repo for tests and the quick start;
  - the **full-body pack** published as a GitHub Release asset and fetched by CMake on demand (`ANYAR_FETCH_ANATOMY=ON`), with SHA-256 verification. This is allowed because CC BY 4.0 permits redistribution.
- [x] **A1.5** Catch2 tests: pack decode, tree integrity, manifest present and licence on the allow-list.

**Exit:** `cmake -DANYAR_FETCH_ANATOMY=ON` produces a working full-body pack, and CI validates the manifest.

## Phase A2 — Anatomy Atlas Explorer MVP (`examples/anatomy-atlas`, ~2 weeks)

*Showcases: SharedBuffer streaming, `run_blocking`, SQLite, events.*

- [x] **A2.1** C++ `AtlasPlugin`:
  - opens the pack (memory-mapped);
  - streams the visible systems into per-system SharedBuffers;
  - `atlas:tree`, `atlas:search` (SQLite FTS5 over FMA names), `atlas:part` (metadata).
- [x] **A2.2** Frontend (three.js):
  - system layer toggles;
  - tree panel and search;
  - click to identify (GPU picking by part ID);
  - isolate, hide and ghost a structure;
  - leader-line labels;
  - camera presets by body region.
- [x] **A2.3** A cross-section clipping plane, with C++ caps (polygon fill of the cut) for solid-looking slices. *Caps are drawn on the GPU with stencil passes instead.*
- [x] **A2.4** A credits line in the viewport and an About dialog generated from the MANIFEST files (not hard-coded).
- [x] **A2.5** Performance budget: the full skeleton plus organs at 60 fps on integrated graphics; record measured numbers in `benchmarks/`.

**Exit:** a demo GIF/video for the README and a screenshot showing the attribution.

### What changed from the plan (A1–A2)

- **One pack, not per-system chunks on disk.** Parts carry a system tag; C++ merges them per system at load time. Simpler format, same streaming behaviour.
- **LODs by vertex clustering in pure Python** (source mesh, 3 mm, 9 mm) instead of meshoptimizer, so the pack builder has no dependencies. It is deterministic: the same inputs give byte-identical packs.
- **The sample is the whole body at 12 mm** (2.8 MB), not a thorax-only pack: the quick start then shows every system.
- **Search is in-memory in C++, not SQLite FTS5.** With 2,234 names it answers in well under 1 ms, and it also matches type/system/region tags ("heart", "artery").
- **The pack is read into memory, not memory-mapped** (≤ 90 MB, read once off the service thread).
- **Clipping caps use stencil passes in WebGL**, not C++ polygon fills.
- **Measured (A2.5):** the whole body at 3 mm (1.69M ▲) holds 60 fps (vsync) on a Radeon 780M iGPU. See `benchmarks/README.md`.

## Phase A3 — Atlas polish & education (~1–2 weeks)

- [ ] **A3.1** An exploded view (per-system radial offsets) and animated transitions between systems.
- [ ] **A3.2** Multilingual labels: English (from FMA) plus Indonesian.
  - ⚠️ **Licensing:** before importing any terminology table, check its licence. The FMA ontology and the Z-Anatomy TA2 CSV each have their own terms, and the Z-Anatomy CSV is BY-SA, so don't commit it. Write our own Indonesian labels, or use sources whose licence has been verified.
- [ ] **A3.3** Quiz mode ("click the left kidney"), with scores stored in SQLite.
- [ ] **A3.4** Optional HRA organ upgrade: swap in higher-detail CC BY 4.0 HRA organs per organ, each with its own manifest entry and attribution.

## Phase B1 — Imaging viewer (`examples/dicom-viewer`, ~2–3 weeks)

*Showcases: large zero-copy buffers (≥ 500 MB volumes), Pinhole native rendering, worker pool.*

- [ ] **B1.1** DICOM loading in C++ through GDCM or DCMTK (confirm the licence and record it in an ADR):
  - series sorting;
  - rescale slope and intercept;
  - orientation and spacing.
- [ ] **B1.2** The volume goes into a SharedBuffer. Three slice views (axial, coronal, sagittal) with window/level presets for bone, lung and soft tissue.
- [ ] **B1.3** WebGL2 3D-texture volume rendering (MIP plus a simple transfer function). Benchmark against Pinhole native rendering for slices.
- [ ] **B1.4** Sample data downloaded at runtime only:
  - Visible Human CT (NLM credit shown in the viewer while it is loaded);
  - optionally TCIA collections after a per-collection licence review (never NC).
- [ ] **B1.5** Privacy: strip or never display patient-identifying DICOM tags in screenshots and exports. Document that users load their own data at their own responsibility.

## Phase B2 — Segmentation & meshing (~2 weeks)

- [ ] **B2.1** Classic C++ tools: threshold, region grow, morphology. Reuse the mics_demo voxel and distance-transform code.
- [ ] **B2.2** Turning a segmentation into a mesh: reuse Surface Nets (or marching cubes, whose patent has expired) and add smoothing and decimation. Run on the worker pool with progress events.
- [ ] **B2.3** TotalSegmentator integration through a Python sidecar or ONNX Runtime:
  - **hard allow-list of Apache-2.0 tasks** (`total`, `total_mr`, `body`, `lung_vessels`, …), enforced in code;
  - licensed tasks (`coronary_arteries`, `heartchambers_highres`, `tissue_types`, `appendicular_bones`, …) are not exposed;
  - `brain_aneurysm` (CC BY-NC) is never exposed;
  - model weights downloaded by the user at runtime, not committed.
- [ ] **B2.4** Overlay the patient's meshes on the atlas (rigid alignment by landmarks or bounding boxes first; ITK registration later).
- [ ] **B2.5** STL/OBJ export for 3D printing, with the disclaimer in the file header.

## Phase B3 — Measurement & planning (~2 weeks)

- [ ] **B3.1** Distance, angle and area measurements on surfaces and slices, with snapping.
- [ ] **B3.2** Vessel centreline plus a diameter profile (geodesic extraction from mics_demo, generalised).
- [ ] **B3.3** Annotations and sessions saved in SQLite. A PDF/HTML report that includes the disclaimer and data credits.

## Phase B4 — Multi-window & release (~1 week)

- [ ] **B4.1** Synced windows: slice views in one window and the 3D view in another, with cursors linked through `emit_to`.
- [ ] **B4.2** Packaging (DEB, AppImage, MSI, NSIS) with NOTICE and licences bundled. The installer licence page shows the code licence plus third-party notices.
- [ ] **B4.3** Release checklist:
  - [ ] Licence facts re-checked against the primary pages; dates updated in this file.
  - [ ] `check_assets.py` green; NOTICE complete.
  - [ ] Disclaimer visible in the README, the app and reports.
  - [ ] No NC, BY-SA or unknown-licence files in the repo or installers (`git ls-files assets/` reviewed).
  - [ ] Credits visible in screenshots and videos used for the README and announcement.

---

## Suggested order and rough effort

A0 (1 d) → A1 (1 w) → A2 (2 w) → **first public showcase** → A3 (1–2 w) → B1 (2–3 w) → B2 (2 w) → B3 (2 w) → B4 (1 w).

The A track (atlas) can ship on its own once A2 is done; the B track (imaging) builds on A1's pack format and A2's renderer.

## Open questions

- ~~Code licence~~ → MIT (ADR-015).
- ~~Full-body pack hosting~~ → GitHub Release asset `anatomy-bodyparts3d-v1` (decided 2026-10-21).
- ~~LibAsyik licence~~ → MIT (checked 2026-10-21, in NOTICE).
- Open Anatomy / SPL: read "3D Slicer License section B" before considering those atlases.
- Labels: which terminology source for multilingual names has a licence compatible with committing it?
- Male/female toggle (shelved 2026-10-21): BodyParts3D is male only. The closest open female source is the HRA female set (CC BY 4.0, Visible Human female frame, `hubmapconsortium/ccf-releases`): uterus, ovaries, fallopian tubes, pelvis, skin, mammary glands, bladder, ureters, kidneys, heart, lungs, liver, vasculature, spinal cord. It has no muscles and no skeleton beyond the pelvis and knees. Options were a composite (BodyParts3D body + HRA female organs, labelled as such) or two separate bodies.
