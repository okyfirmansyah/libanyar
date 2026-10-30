# BodyParts3D anatomy data

> **BodyParts3D, © The Database Center for Life Science licensed under CC Attribution 4.0 International**
> Source: https://dbarchive.biosciencedbc.jp/en/bodyparts3d/ · Licence: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) ([LICENSE](LICENSE))

The data in this folder is **not** covered by LibAnyar's MIT licence.

| File | What | Size |
|---|---|---|
| `sample.pack` | All 2,234 BodyParts3D 4.0 parts, simplified at 12 mm (≈253k triangles). Embedded in the app so it works out of the box. | 2.8 MB |
| `bodyparts3d-full.pack` (not in Git) | The same parts at three levels of detail: source mesh (6.7M triangles), 3 mm (1.7M) and 9 mm (0.4M). Published as a GitHub Release asset and fetched with `-DANYAR_FETCH_ANATOMY=ON`. | 90 MB |

`MANIFEST.json` records the source archives' SHA-256 values, the exact tool invocation, and the full pack's URL and SHA-256.

## Licence notes (checked 2026-10-21)

- The [BodyParts3D licence page](https://dbarchive.biosciencedbc.jp/en/bodyparts3d/lic.html) (last updated 2025-02-27) states **CC BY 4.0**. The OBJ headers inside the 4.0 archives still say CC BY-SA 2.1 JP; the website is the newer, authoritative statement.
- Attribution must keep the exact wording above. The app shows it in the viewport and in its About dialog.
- Changes made: OBJ → cm, re-framed (+x patient left, +y up, +z anterior, soles at y = 0), simplified by vertex clustering, 16-bit quantised, and classified into body systems and regions using the BodyParts3D is-a / part-of lists.

## Rebuilding

```bash
B=https://dbarchive.biosciencedbc.jp/data/bodyparts3d/LATEST
mkdir bp3d && cd bp3d
for f in isa_BP3D_4.0_obj_99.zip isa_inclusion_relation_list.txt isa_element_parts.txt \
         partof_inclusion_relation_list.txt partof_element_parts.txt; do curl -LO $B/$f; done
mkdir obj && unzip -q isa_BP3D_4.0_obj_99.zip -d obj && cd ..
python3 examples/anatomy-atlas/tools/build_atlas_pack.py bp3d sample.pack --lods 12 --only-lod 1
python3 examples/anatomy-atlas/tools/build_atlas_pack.py bp3d bodyparts3d-full.pack
```

The build is deterministic: the same inputs give byte-identical packs.
