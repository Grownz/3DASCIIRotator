# Changelog

All notable changes to **3D ASCII Rotator** are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.2.0] - 2026-10-07

### Added
- User-supplied models: drop `.stl`, `.obj` or `.ply` files into a `models/`
  folder next to the executable and they are added to the `SPACE` cycle. The
  folder is rescanned live (about once a second); `R` forces a rescan.
- STL (binary + ASCII), OBJ and PLY (ASCII + binary little-endian) readers,
  with a 128 MB / 500k-triangle cap and an LRU cache of the last three BVHs.
- Optional per-model sidecar `<name>.json` (up axis, yaw, pitch, elevation,
  zoom, display name).
- Adaptive quality: meshes above ~100k triangles skip the 2x2 supersampling.

### Changed
- The shape list is dynamic now; `-s` also accepts names of loaded models.
- Mesh indices are 32-bit, so meshes are no longer limited to 65k vertices.

## [0.1.0] - 2026-10-07

### Added
- `SPACE` cycles through all shapes in insertion order at runtime.
- The available shapes are defined as a single ordered list, shared by the
  `--shape` option, the help text and the runtime cycling.

## [0.0.7] - 2026-10-07

### Added
- New shape `china` — a "Tank Man" scene (a person with shopping bags standing
  in front of a Type 59 tank). Aliases: `tiananmen`, `tankman`, `tank`.
- The figure and the tank are produced by `tools/make_shape_models.py`.

## [0.0.6] - 2026-10-07

### Added
- Three new shapes, each an embedded triangle mesh:
  - `fish` — a trout;
  - `kebab` — a skewer kebab;
  - `berlin` — the Brandenburg Gate.
  Aliases include `trout`/`forelle`, `doner`/`doener`, and `tor`/`brandenburg`/`gate`.
- `tools/make_shape_models.py` produces these meshes.

## [0.0.5] - 2026-10-06

### Added
- Three new shapes, each an embedded triangle mesh:
  - `f1` — a Formula-style open-wheel race car;
  - `companion` — the Portal companion cube;
  - `maus` — "Die Maus".
  Aliases include `formula`/`race`, `portal`/`companioncube` and `mouse`.
- `tools/make_shape_models.py` to regenerate those embedded meshes.

### Changed
- The mesh renderer is now generic: it handles any number of embedded meshes and
  automatically scales each one to fill the view.

## [0.0.4] - 2026-10-06

### Changed
- The stegosaurus is embedded as a BVH-accelerated triangle mesh with flat
  shading, which preserves its back plates, tail spikes and legs.
- `tools/make_stego_model.py` produces the stego mesh.

## [0.0.3] - 2026-10-06

### Added
- New shape `stego` (aliases `stegosaurus`, `dinosaur`): an embedded, shaded 3D
  model of a *Stegosaurus*, rendered with the ray-tracing pipeline.
- A reproducible generator script for the stego mesh.

## [0.0.2] - 2026-10-06

### Added
- Tilt the rotation axis with the `Up` / `Down` arrow keys: `Up` tips the axis
  away from the viewer, `Down` tips it towards the viewer, in 5 degree steps
  up to a maximum of 90 degrees (either way).
- `ENTER` shatters the object: every visible character becomes a particle that
  falls under gravity onto the invisible floor the object was resting on,
  bounces off the ground and its neighbours, and settles into a shallow debris
  layer. Press `ENTER` again to rebuild the object.
- `--tilt <deg>` option to set the initial axis tilt.
- `--shatter` and `--sim <sec>` options to render and simulate the shattered
  state in `--snapshot` mode.

### Changed
- The shading ramp was widened from 10 to 15 levels
  (`" .,:;~-=+*oO#%@"`) for smoother gradients.

## [0.0.1] - 2026-10-06

### Added
- Initial release.
- Native Windows x64 console application written in C (no runtime dependencies).
- `-s` / `--shape` option to select one solid per run:
  `cube`, `cylinder`, `diamond` (octahedron) and `sphere`.
- Real 3D rendering via signed-distance-field ray marching, shaded with
  Blinn-Phong diffuse + specular lighting, hemispheric ambient light,
  a fresnel rim light and SDF ambient occlusion.
- Luminance mapped to a 10-level ASCII density ramp, with 2x2 supersampling
  per character cell for smoother silhouettes.
- Continuous rotation around the vertical (z) axis, starting at 15 deg/s.
- Live spin control: `+` raises the speed by 5 deg/s (up to 90 deg/s),
  `-` lowers it by 5 deg/s (down to 5 deg/s).
- `ESC` (or `q`) quits and restores the console.
- Automatic handling of console resizing and alternate-screen-buffer restore.
- `--snapshot` mode to render a single frame to stdout (useful for testing).
- `--help` and `--version` output.
- Released under the MIT License.
