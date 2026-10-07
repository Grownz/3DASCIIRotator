# Feature spec: user-supplied models (0.2.0)

Drop 3D files into a `models/` folder next to `ascii3D.exe` and they appear in
the shape list, cycled with `SPACE` like the built-in shapes.

## Decisions

| Topic | Decision |
| --- | --- |
| Formats | STL (binary + ASCII), OBJ, PLY (ASCII + binary little-endian) |
| Limits | file <= 128 MB, triangles <= 500k (otherwise skipped + warning) |
| Orientation | assume Z-up; interactive tilt still applies |
| Folder / live | `models/` next to the exe, polled ~1 s, appended to the cycle |
| BVH memory | LRU cache of 3 BVHs (most recently shown) |
| Quality | adaptive: 2x2 supersampling up to ~100k triangles, then 1x |
| Metadata | optional sidecar `name.json` |

## Behaviour

- `models/` is created on start if missing; only top-level files are scanned.
- Startup scan + polling: new file -> register (parse lazily); changed
  (mtime/size) -> reload; removed -> drop.
- Order: built-in shapes first, then loaded models alphabetically. `SPACE`
  cycles through everything; the order stays stable.
- Names: display name and `-s <stem>` are the file name without extension
  (overridable via sidecar); collisions get a `-2`, `-3`, ... suffix.
- `R` forces a rescan (in addition to the automatic poll).
- `--snapshot` reads `models/` too; `--snapshot -s <stem>` works.
- `models/` is git-ignored (user data; the repo stays MIT-clean).

## Sidecar (`models/<name>.json`, optional)

```json
{
  "up":    "z",        // z | y | x -> which model axis points up (default "z")
  "yaw":   0,          // degrees, extra rotation about the up axis
  "pitch": 0,          // degrees, extra tilt
  "elev":  15,         // camera elevation for this model (default 15)
  "zoom":  1.0,        // multiplier on the auto-fit scale
  "name":  "My Model"  // display / CLI name (default: file stem)
}
```

An invalid sidecar is ignored with a warning; the model still loads.

## Limits and error handling

- Over the file/triangle limit -> skip the file, one-line notice, keep running.
- Broken file (truncated binary STL: count vs file size; bad PLY header; empty
  file) -> skip + notice.
- NaN/Inf/degenerate triangles / zero-size bounding box -> drop those; if
  nothing remains, skip.
- Unicode and long paths via wide APIs; extension match is case-insensitive.
- File being written, or removed while selected -> retry / fall back.
- Too many files -> cap (32) + notice.

## Implementation notes

- Index type changes from `unsigned short` to `unsigned int`; both generators
  (`tools/make_stego_model.py`, `tools/make_shape_models.py`) emit `unsigned int`
  and the embedded headers are regenerated.
- `MeshDef` gains `owned` (malloc vs `static const`) and `zoom`.
- The fixed `SHAPE_NAMES[]` list is replaced by a dynamic registry of items
  (analytic or mesh); `SH_LOADED` is a generic mesh kind.
- New module `src/loader.c/.h` implements the STL/OBJ/PLY parsers
  (bounds-checked, parsed in `double`, normalised to a unit-ish box).
- Scanning/polling and the LRU(3) BVH cache live in `main.c`.
