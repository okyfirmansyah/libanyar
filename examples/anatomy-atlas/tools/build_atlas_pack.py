#!/usr/bin/env python3
"""Build a whole-body atlas pack (ANATPAK2) from the BodyParts3D 4.0 release.

Data: BodyParts3D, © The Database Center for Life Science licensed under CC
Attribution 4.0 International. Licence page (checked 2026-10-21, last updated
2025-02-27): https://dbarchive.biosciencedbc.jp/en/bodyparts3d/lic.html
The OBJ headers inside the 4.0 archives still say CC BY-SA 2.1 JP; the
website is the authoritative, newer statement.

Inputs (from https://dbarchive.biosciencedbc.jp/data/bodyparts3d/LATEST/):
    isa_BP3D_4.0_obj_99.zip (unzipped), isa_inclusion_relation_list.txt,
    isa_element_parts.txt, partof_inclusion_relation_list.txt,
    partof_element_parts.txt

Usage (pure Python 3, no dependencies):
    python3 build_atlas_pack.py <bp3d_dir> <out.pack> [--lods 3,9] [--only-lod 1]

    <bp3d_dir> holds the .txt lists and obj/isa_BP3D_4.0_obj_99/*.obj.
    --lods      vertex-clustering cell sizes in mm for LOD1.. (LOD0 = source mesh)
    --only-lod  keep just this LOD (e.g. a small sample pack)
    Also writes <out.pack>.manifest.json next to the pack.

Pack layout (little-endian, every blob 4-byte aligned):
    "ANATPAK2" | u32 header_len | header JSON (UTF-8, space-padded to 4) | blobs
    header.parts[i] = {id, fma, name, system, kind, region, min[3], max[3],
                       lods: [{vertices, triangles, positions, indices, index_bits}]}
    Offsets are relative to the start of the blobs. Positions are u16 xyz;
    a value decodes as min + q / 65535 * (max - min).

Frame: centimetres, +x = patient's left, +y = up, +z = anterior; x and z
centred on the body, y = 0 at the soles.
"""

import collections
import glob
import hashlib
import json
import os
import struct
import sys

SOURCE = {
    "name": "BodyParts3D",
    "version": "4.0 (isa, obj_99)",
    "license": "CC-BY-4.0",
    "license_url": "https://creativecommons.org/licenses/by/4.0/",
    "attribution": "BodyParts3D, © The Database Center for Life Science licensed under CC Attribution 4.0 International",
    "source_url": "https://dbarchive.biosciencedbc.jp/en/bodyparts3d/",
    "license_page": "https://dbarchive.biosciencedbc.jp/en/bodyparts3d/lic.html",
}

SYSTEMS = ["skeletal", "muscular", "cardiovascular", "nervous", "respiratory", "digestive", "urogenital",
           "integumentary", "other"]

# is-a ancestors → (system, kind); first match wins.
ISA_RULES = [
    ({"FMA50720", "FMA30313", "FMA63812"}, "cardiovascular", "artery"),
    ({"FMA50723", "FMA30314", "FMA63814"}, "cardiovascular", "vein"),
    ({"FMA50722"}, "cardiovascular", "vessel"),
    ({"FMA65132", "FMA11195"}, "nervous", "nerve"),
    ({"FMA256237", "FMA83143"}, "nervous", "brain"),
    ({"FMA12224"}, "respiratory", "airway"),
    ({"FMA5022", "FMA85453", "FMA10474", "FMA32558"}, "muscular", "muscle"),
    ({"FMA5018"}, "skeletal", "bone"),
    ({"FMA55107"}, "skeletal", "cartilage"),
    ({"FMA12516"}, "digestive", "tooth"),
]
# part-of ancestors → (system, kind)
PARTOF_RULES = [
    ({"FMA7088"}, "cardiovascular", "heart"),
    ({"FMA7161", "FMA228642"}, "cardiovascular", "vessel"),
    ({"FMA7157"}, "nervous", "brain"),
    ({"FMA7309", "FMA7310"}, "respiratory", "lung"),
    ({"FMA7158"}, "respiratory", "airway"),
    ({"FMA7152"}, "digestive", "organ"),
    ({"FMA23881"}, "skeletal", "bone"),
]
# name keywords → (system, kind) for elements outside both rule sets.
NAME_RULES = [
    (("skin", "hair", "eyebrow", "lip", "external ear"), "integumentary", "skin"),
    (("kidney", "ureter", "bladder", "urethra", "testis", "epididymis", "deferent", "seminal", "prostate", "penis",
      "uterus", "ovary", "vagina"), "urogenital", "organ"),
    (("liver", "sublingual", "parotid", "submandibular", "gallbladder"), "digestive", "organ"),
    (("epiglottis", "conus elasticus", "larynx", "laryng"), "respiratory", "airway"),
    (("artery", "arterial", "palmar arch"), "cardiovascular", "artery"),
    (("vein", "venous"), "cardiovascular", "vein"),
    (("ventricle", "atrium", "heart"), "cardiovascular", "heart"),
    (("nerve", "thalamus", "cerebr", "pituitary", "choroid", "foramen", "fossa", "tentorium", "retina"), "nervous", "brain"),
    (("tendon", "ligament", "membrane", "tract", "retinaculum", "raphe", "trapezius", "linea alba", "levator", "tendinous"),
     "muscular", "tendon"),
    (("spleen", "gland", "thyroid", "adrenal", "thymus"), "other", "organ"),
    (("eye", "lacrimal", "ciliar", "rectus", "oblique", "palpebr", "tarsal plate", "lens", "cornea"), "other", "eye"),
]


def relations(path):
    par = collections.defaultdict(list)
    for line in open(path, encoding="utf-8"):
        c = line.rstrip("\n").split("\t")
        if len(c) >= 4 and c[0].startswith("FMA"):
            par[c[2]].append(c[0])
    return par


def element_concepts(path):
    m = collections.defaultdict(set)
    for line in open(path, encoding="utf-8"):
        c = line.rstrip("\n").split("\t")
        if len(c) >= 3 and c[0].startswith("FMA"):
            m[c[2]].add(c[0])
    return m


def ancestors(par, starts):
    seen, stack = set(starts), list(starts)
    while stack:
        for p in par.get(stack.pop(), []):
            if p not in seen:
                seen.add(p)
                stack.append(p)
    return seen


def load_obj(path):
    vs, fs, meta = [], [], {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("v "):
                _, x, y, z = line.split()[:4]
                vs.append((float(x), float(y), float(z)))
            elif line.startswith("f "):
                idx = [int(t.split("/")[0]) - 1 for t in line.split()[1:]]
                for k in range(1, len(idx) - 1):
                    fs.append((idx[0], idx[k], idx[k + 1]))
            elif line.startswith("# Concept ID"):
                meta["fma"] = line.split(":", 1)[1].strip()
            elif line.startswith("# English name"):
                meta["name"] = line.split(":", 1)[1].strip()
    return vs, fs, meta


def cluster(vs, fs, cell):
    """Vertex-clustering simplification (cell in cm). Returns (vs, fs)."""
    cells, remap, acc = {}, [], []
    for v in vs:
        key = (int(v[0] // cell), int(v[1] // cell), int(v[2] // cell))
        i = cells.get(key)
        if i is None:
            i = cells[key] = len(acc)
            acc.append([0.0, 0.0, 0.0, 0])
        a = acc[i]
        a[0] += v[0]; a[1] += v[1]; a[2] += v[2]; a[3] += 1
        remap.append(i)
    seen, nf = set(), []
    for a, b, c in fs:
        a, b, c = remap[a], remap[b], remap[c]
        if a == b or b == c or a == c:
            continue
        key = (a, b, c) if a < b and a < c else (b, c, a) if b < c else (c, a, b)
        if key in seen:
            continue
        seen.add(key)
        nf.append((a, b, c))
    used = sorted({i for f in nf for i in f})
    m = {o: n for n, o in enumerate(used)}
    return [(acc[i][0] / acc[i][3], acc[i][1] / acc[i][3], acc[i][2] / acc[i][3]) for i in used], \
           [(m[a], m[b], m[c]) for a, b, c in nf]


def classify(fj, name, iel, ipar, pel, ppar):
    a = ancestors(ipar, iel.get(fj, set()))
    for ids, system, kind in ISA_RULES:
        if a & ids:
            return system, kind
    b = ancestors(ppar, pel.get(fj, set()))
    for ids, system, kind in PARTOF_RULES:
        if b & ids:
            return system, kind
    low = name.lower()
    for words, system, kind in NAME_RULES:
        if any(w in low for w in words):
            return system, kind
    return "other", "organ"


def region(c, height):
    """Coarse body region from a part's centre (scene cm, y = 0 at the soles)."""
    x, y = abs(c[0]), c[1] / height
    if y > 0.82:
        return "head"
    if x > 16.5 and y > 0.35:          # arms hang beside the thighs in the anatomical pose
        return "upper_limb"
    if y < 0.47:
        return "lower_limb"
    if x > 15.5 or (y < 0.60 and x > 13.0):
        return "upper_limb"
    if y > 0.685:                       # ≈ the diaphragm
        return "thorax"
    if y > 0.55:
        return "abdomen"
    return "pelvis"


def main():
    args = sys.argv[1:]
    lods_mm, only = [3.0, 9.0], None
    if "--lods" in args:
        i = args.index("--lods")
        lods_mm = [float(x) for x in args[i + 1].split(",")]
        del args[i:i + 2]
    if "--only-lod" in args:
        i = args.index("--only-lod")
        only = int(args[i + 1])
        del args[i:i + 2]
    bp3d, out = args[0], args[1]

    ipar = relations(f"{bp3d}/isa_inclusion_relation_list.txt")
    ppar = relations(f"{bp3d}/partof_inclusion_relation_list.txt")
    iel = element_concepts(f"{bp3d}/isa_element_parts.txt")
    pel = element_concepts(f"{bp3d}/partof_element_parts.txt")

    # Load + re-frame: (x, y, z) mm → (x, z, -y) cm.
    files = sorted(glob.glob(f"{bp3d}/obj/isa_BP3D_4.0_obj_99/*.obj"))
    raw = []
    for path in files:
        vs, fs, meta = load_obj(path)
        if not fs:
            continue
        vs = [(v[0] / 10.0, v[2] / 10.0, -v[1] / 10.0) for v in vs]
        raw.append((os.path.basename(path)[:-4], meta, vs, fs))
    lo = [min(min(v[i] for v in r[2]) for r in raw) for i in range(3)]
    hi = [max(max(v[i] for v in r[2]) for r in raw) for i in range(3)]
    off = [-(lo[0] + hi[0]) / 2, -lo[1], -(lo[2] + hi[2]) / 2]
    height = hi[1] - lo[1]

    header = {"format": "ANATPAK2", "units": "cm",
              "frame": "+x patient left, +y up, +z anterior; x,z centred on the body, y=0 at the soles",
              "height": round(height, 2), "source": SOURCE, "systems": SYSTEMS,
              "lods_mm": ([0.0] + lods_mm) if only is None else [([0.0] + lods_mm)[only]], "parts": []}
    blobs = bytearray()
    counts = collections.Counter()
    tri_total = [0] * (len(lods_mm) + 1)

    def put(data):
        at = len(blobs)
        blobs.extend(data)
        while len(blobs) % 4:
            blobs.append(0)
        return at

    for n, (fj, meta, vs, fs) in enumerate(raw):
        vs = [(v[0] + off[0], v[1] + off[1], v[2] + off[2]) for v in vs]
        name = meta.get("name") or fj
        system, kind = classify(fj, name, iel, ipar, pel, ppar)
        mn = [min(v[i] for v in vs) for i in range(3)]
        mx = [max(v[i] for v in vs) for i in range(3)]
        centre = [(mn[i] + mx[i]) / 2 for i in range(3)]
        levels = [(vs, fs)]
        for mm in lods_mm:
            cell = mm / 10.0
            lv, lf = cluster(vs, fs, cell)
            while not lf and cell > 0.05:       # tiny part vanished: retry finer
                cell /= 2
                lv, lf = cluster(vs, fs, cell)
            levels.append((lv, lf) if lf else levels[-1])
        if only is not None:
            levels = [levels[only]]
        span = [max(mx[i] - mn[i], 1e-6) for i in range(3)]
        lods = []
        for li, (lv, lf) in enumerate(levels):
            pos = bytearray()
            for v in lv:
                pos += struct.pack("<3H", *(round((v[i] - mn[i]) / span[i] * 65535) for i in range(3)))
            bits = 16 if len(lv) < 65536 else 32
            idx = bytearray()
            fmt = "<3H" if bits == 16 else "<3I"
            for f in lf:
                idx += struct.pack(fmt, *f)
            lods.append({"vertices": len(lv), "triangles": len(lf), "positions": put(pos),
                         "indices": put(idx), "index_bits": bits})
            tri_total[li if only is None else 0] += len(lf)
        header["parts"].append({
            "id": fj, "fma": meta.get("fma", ""), "name": name, "system": system, "kind": kind,
            "region": "whole_body" if system == "integumentary" else region(centre, height),
            "min": [round(x, 4) for x in mn], "max": [round(x, 4) for x in mx], "lods": lods,
        })
        counts[system] += 1
        if n % 200 == 0:
            print(f"  {n}/{len(raw)} …", file=sys.stderr)

    hj = json.dumps(header, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    hj += b" " * (-len(hj) % 4)
    with open(out, "wb") as f:
        f.write(b"ANATPAK2" + struct.pack("<I", len(hj)) + hj + blobs)
    digest = hashlib.sha256(open(out, "rb").read()).hexdigest()
    print(f"wrote {out}: {os.path.getsize(out)} bytes, {len(raw)} parts, triangles per LOD {tri_total[:len(header['lods_mm'])]}")
    print("systems:", dict(counts))
    manifest = dict(SOURCE)
    manifest.update({
        "retrieved": "2026-10-21",
        "inputs": {os.path.basename(p): hashlib.sha256(open(p, "rb").read()).hexdigest()
                   for p in sorted(glob.glob(f"{bp3d}/*.zip")) + sorted(glob.glob(f"{bp3d}/*.txt"))},
        "output": {"file": os.path.basename(out), "sha256": digest, "bytes": os.path.getsize(out),
                   "parts": len(raw), "lods_mm": header["lods_mm"]},
        "changes": "converted from OBJ, re-framed to cm, simplified by vertex clustering, positions quantised to 16 bits",
        "tool": "examples/anatomy-atlas/tools/build_atlas_pack.py <bp3d_dir> " + os.path.basename(out) + "".join(
            f" {a}" for a in sys.argv[3:]),
        "note": "The OBJ headers in the 4.0 archive still state CC BY-SA 2.1 JP; the licence page (updated 2025-02-27) states CC BY 4.0.",
    })
    with open(out + ".manifest.json", "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")


if __name__ == "__main__":
    main()
