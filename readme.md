# 3D ASCII Rotator

A tiny native Windows x64 console application that renders shaded **3D solids
as animated ASCII art**. No runtime, no third-party libraries, just one small
native `.exe`.

```
                                                       :%-  :
                                                       ~###Ooo=   *O
                                                 *#O.  ~O#Oooo** OOO-  -%~;
                                                +OoooOOOOo******oOoOO o#**o.
                                          :    #oooo**OOOooo****ooo*OOoooooo
                                         -**oO:OOOOOO##%ooooooooooo+Ooooooo:
                                         ooooO*O###@@@@@####OOOO###oOOOOOoo ~-:
                                      +o~,oooO%%#%%@@@@@OO%%%OOOoooOOOOOOOOO##O *~:
                                    ~ ##Oo##%%@%#%%%@@@@Oo+**oooooOOOOOOOoO#####**o
                               ~   +OOOOO%@@%%%%%#@@@@@#Ooo-ooooooOOOOOOO#OOOOoooo+
          ~                   oooOO*#@@@@%%OOOOOoO@@@@@oooo*****ooOOOOOOO###o+++*o*#o
         ~ - :.               ,o*+o%%####Oo**ooOOo%###Oooo+***oooooooOO####o*****oOoo~
          ~:o~:-~          **- ~o####Oo*****--...#####ooo=+**oooooooooOOo*+#OOooOO#%%@@O;
            =Ooo=o-,       +##%##%#***-;;,     -OOOOO*++:================-+####+++o=+ooooOo;
              ;;*+o##@%########*+~:           :OOOOo++*#oo=               ;oOOO
                      ,;;;,                   -OOO**=##o*,                 oOO
                                              OOO*+.%%%o                   ooo
                                              OOO+;-%%#*:                  ooo
                                              @@#o                        ~#O=~
                                             %%%Oo.                       ~##o
                                             :--~;~
```

## Download

Prebuilt Windows x64 binaries are available on the
[releases page](https://github.com/Grownz/3DASCIIRotator/releases/latest).
Latest: [ascii3d-0.1.0-win-x64.exe](https://github.com/Grownz/3DASCIIRotator/releases/download/v0.1.0/ascii3d-0.1.0-win-x64.exe).
No installer and no runtime are required.

## Features

- **Real 3D rendering.** Objects are described as signed distance fields and
  rendered by ray marching, so silhouettes and lighting are computed in three
  dimensions rather than faked in 2D.
- **High-quality shading.** Each character cell gets a surface normal and is
  lit with Blinn-Phong diffuse + specular highlights, a hemispheric ambient
  term, a fresnel rim light and distance-field ambient occlusion. Luminance is
  mapped onto a 15-level ASCII density ramp and every cell is supersampled 2x2
  for smooth edges.
- **Twelve shapes.** Cube, cylinder, diamond (octahedron), sphere, a
  stegosaurus, a Formula-style race car, a companion cube, "Die Maus", a trout,
  a kebab, the Brandenburg Gate and a "Tank Man" scene (the latter eight are
  embedded, ray-traced triangle meshes — see *Third-party assets*).
- **Cycle at runtime.** The shapes are kept in a fixed list in insertion order;
  press `SPACE` to switch to the next one without restarting.
- **Always spinning.** The solid rotates around the vertical (z) axis at
  15 deg/s by default.
- **Tiltable spin axis.** Press `Up` / `Down` to tip the spin axis away from or
  towards the viewer in 5 deg steps, up to 90 deg.
- **Live speed control.** Press `+` / `-` to change the spin in 5 deg/s steps
  between 5 and 90 deg/s while it runs.
- **Shatter physics.** Press `Enter` to break the solid apart: every character
  becomes a particle that falls under gravity and piles up on the invisible
  floor the object was resting on. Press `Enter` again to rebuild it.
- **Clean exit.** `ESC` restores your console exactly as it was.
- **Tiny and dependency-free.** Plain C99 compiled with MSVC straight to a
  native Windows x64 executable.

## Requirements

- Windows 10 or newer (x64). Windows Terminal or the classic console both work
  (ANSI/VT escape sequences must be supported).
- To build: Visual Studio 2022 or the Visual Studio Build Tools with the
  "Desktop development with C++" workload (MSVC compiler + Windows SDK). No
  other runtime is required. Regenerating the embedded models additionally
  needs Python 3 with numpy and trimesh (and pygltflib for the stegosaurus).

## Build

From the project root, run:

```bat
build.bat
```

This locates your Visual Studio installation, initializes the MSVC
environment and produces `build\ascii3d.exe`.

## Usage

```bat
build\ascii3d.exe -s cube
```

### Options

| Option | Description |
| --- | --- |
| `-s`, `--shape <name>` | Shape to render: `cube`, `cylinder`, `diamond`, `sphere`, `stego`, `f1`, `companion`, `maus`, `fish`, `kebab`, `berlin`, `china` (default: `cube`). |
| `-h`, `--help` | Show help and exit. |
| `-v`, `--version` | Show the version and exit. |
| `--angle <deg>` | Initial rotation angle in degrees (default: `0`). |
| `--tilt <deg>` | Initial spin-axis tilt in degrees, `-90`..`90` (default: `0`). |
| `--snapshot` | Render a single frame as plain text to stdout and exit. |
| `--shatter` | With `--snapshot`: shatter the solid and simulate the fall. |
| `--sim <sec>` | With `--shatter`: how many seconds to simulate (default: `3`). |

### Examples

```bat
build\ascii3d.exe -s sphere
build\ascii3d.exe --shape diamond
build\ascii3d.exe -s cylinder --angle 30
build\ascii3d.exe -s cube --tilt 45
build\ascii3d.exe -s stego
build\ascii3d.exe -s f1 --tilt 20
build\ascii3d.exe -s companion
build\ascii3d.exe -s maus
build\ascii3d.exe -s fish
build\ascii3d.exe -s kebab
build\ascii3d.exe -s berlin --tilt 15
build\ascii3d.exe -s china
build\ascii3d.exe --snapshot -s cube > frame.txt
build\ascii3d.exe --snapshot -s stego --angle 90
build\ascii3d.exe --snapshot -s f1 --shatter --sim 2 > settled.txt
```

## Controls

| Key | Action |
| --- | --- |
| `ESC` | Quit. |
| `+` | Increase spin speed by 5 deg/s (maximum 90 deg/s). |
| `-` | Decrease spin speed by 5 deg/s (minimum 5 deg/s). |
| `Up` / `Down` | Tilt the spin axis away from / towards the viewer in 5 deg steps (maximum 90 deg). |
| `Enter` | Shatter the solid; press again to rebuild it. |
| `Space` | Switch to the next shape (insertion order). |
| `q` | Quit (convenience alias for `ESC`). |

## How it works

Each character cell maps to a ray cast into the scene. A signed distance
function (`sdf`) describes the selected solid; ray marching walks along the
ray until it hits the surface. The camera sits slightly above the horizon so
the top faces and caps of the solids are visible, which makes the 3D form easy
to read. The surface normal is obtained from the distance-field gradient, and
the point is shaded with:

1. a hemispheric **ambient** term (brighter from above),
2. **Blinn-Phong diffuse** light from the upper-left front,
3. a tight **specular** highlight,
4. a **fresnel rim** light along the silhouette,
5. distance-field **ambient occlusion** for contact shading.

The resulting luminance drives the 15-level ASCII ramp `" .,:;~-=+*oO#%@"`,
and each cell is sampled 2x2 to smooth the edges. The object is rotated about
the vertical `z` axis by integrating the current spin speed over real elapsed
time, so the motion speed is independent of the frame rate. The `Up` / `Down`
keys tilt that axis about the horizontal `x` axis.

The `stego`, `f1`, `companion` and `maus` shapes are not analytic
primitives: each is an embedded **triangle mesh** ray-traced with a
bounding-volume hierarchy and shaded flat. The meshes are automatically
scaled to fill the current view (accounting for perspective), and each uses a
camera elevation that suits it. The stegosaurus keeps its sharp back plates
and tail spikes; the F1 car its exposed wheels and wings.

When you press `Enter`, the current characters are turned into particles with a
small random outward kick. Each particle is integrated with gravity and air
friction, bounces with restitution off the ground and its neighbours, and then
settles into a shallow debris layer on the floor the object was resting on.

## Project layout

```
3DASCIIRotator/
  src/main.c            application source (single file)
  src/stego_model.h     embedded stegosaurus mesh (generated)
  src/f1_model.h        embedded F1 car mesh (generated)
  src/companion_model.h embedded companion cube mesh (generated)
  src/maus_model.h      embedded "Die Maus" mesh (generated)
  src/fish_model.h      embedded trout mesh (generated)
  src/kebab_model.h     embedded kebab mesh (generated)
  src/berlin_model.h    embedded Brandenburg Gate mesh (generated)
  src/china_model.h     embedded "Tank Man" scene mesh (generated)
  tools/make_stego_model.py    regenerates src/stego_model.h
  tools/make_shape_models.py   regenerates the other model headers
  build.bat             MSVC build script
  readme.md             this file
  changelog.md          version history
```

The generated headers are committed, so a normal build only needs MSVC. To
regenerate them (needs Python 3 with numpy and trimesh; the stegosaurus
additionally needs pygltflib; the source models are downloaded on demand):

```bat
python tools\make_stego_model.py
python tools\make_shape_models.py
```

## Third-party assets

- **Stegosaurus** — model by **[Quaternius](https://quaternius.com)**, licensed
  [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/), via
  [poly.pizza](https://poly.pizza/m/eFcNbOlpvl). Skinned, re-oriented and
  scaled by `tools/make_stego_model.py`.
- **F1 car** — the "race" car from **[Kenney](https://kenney.nl/assets/car-kit)**'s
  Car Kit, licensed
  [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/). A generic
  open-wheel Formula-style car; it is not a replica of any specific 2008 team.
- **Companion cube** — "Portal: Companion Cube" by *Prateek Karajgikar* via
  [poly.pizza](https://poly.pizza/m/ccQRPuBwXrU), licensed
  [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) (attribution
  required). *Portal* and the Companion Cube are trademarks of Valve; this is
  fan-made geometry.
- **"Die Maus"** — no permissively-licensed model was available, so this one is
  original geometry built from primitives by `tools/make_shape_models.py`. It is
  an homage to the WDR character and is not an official model.
- **Trout** — "Trout" by **Poly by Google** via
  [poly.pizza](https://poly.pizza/m/2W2sKWYvk8k), licensed
  [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) (attribution
  required).
- **Kebab** — "Kebab" by **Poly by Google** via
  [poly.pizza](https://poly.pizza/m/47UYz6vLa_V), licensed
  [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) (attribution
  required). It is a skewer kebab; no free *Döner* sandwich model was available.
- **Brandenburg Gate** — original geometry built from primitives by
  `tools/make_shape_models.py` (colonnade, entablature and quadriga); no
  permissively-licensed model was available.
- **"Tank Man" (`china`)** — the figure is from the **"Tiananmen Square
  Playset"** by *WindhamGraves* ([Thingiverse thing 6007266](https://www.thingiverse.com/thing:6007266));
  the Type 59 tank is original geometry. Both are baked by
  `tools/make_shape_models.py`.

The meshes are baked into the C headers listed above; no model file is loaded
at runtime.

## Versioning

Current version: **0.1.0**. See [changelog.md](changelog.md) for details.

## Roadmap

- `0.1.0` — open the GitHub repository and continue development there.

## License

Released under the **MIT License**. See [LICENSE](LICENSE) for the full text.

Copyright (c) 2026 Grownz.
