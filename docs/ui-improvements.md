# UI ideas (backlog)

A running list of possible UI/UX improvements, noted for later. **Nothing here
is scheduled or implemented yet** — the file only records the ideas so they are
not lost. Effort/impact are rough.

| Idea | Area | Effort | Impact |
| --- | --- | --- | --- |
| Move the key hints out of the HUD into a help overlay (`?` / `F1`) | HUD | S | high |
| HUD on/off (`H`) + compact mode; drop the version from it | HUD | S | high |
| Filter/search in the model list (type to filter) | list | M | high |
| Hex/RGB label + reset for the colour sliders | sliders | S | medium |
| Mark the *currently loaded* model in the list (not just the cursor) | list | S | medium |
| Group the list into "built-in" vs "models/" | list | S | medium |
| Export progress (`frame n/N`) + cancel | feedback | M | medium |
| Loading indicator for large models | feedback | M | medium |
| Pause key; reverse shape cycle; direct shape jump (1-9) | input | S | medium |
| Remember settings between runs (shape, speed, tilt, colours, FPS) | settings | M | medium |
| Mouse drag to rotate / tilt | input | L | medium |
| Ramp / theme presets | rendering | M | medium |
| Quality toggle (supersampling) in the UI | rendering | S | medium |
| `[` / `]` as alternatives to `+` / `-` | input | S | low |
| Real scrollbar + clearer truncation marker in the list | list | S | low |
| Configurable / remembered panel width | list | M | low |

## HUD / status line

Currently one always-on line, top-left:
`3D ASCII Rotator v0.2.20 | shape: cube | spin: 15 deg/s | tilt: 0 deg  [ESC] quit  [-/+] speed ...`

- Move the key hints out of the HUD into a help overlay; the HUD then only
  shows state (shape, spin, tilt).
- Toggle the HUD (`H`) and/or auto-hide it after a few seconds, reappearing on
  change.
- Drop the version number from the HUD (it belongs to `--version`).
- Compact mode: `cube  15 deg/s  tilt 0 deg`, selectable via `--hud full|compact|off`.
- Add per-mesh info: triangle count and LOD level (`LOD 3/7`), optionally `ms/frame`.

## Help & discoverability

- Help overlay (`?` / `F1`) listing every binding, dismissed with any key.
- First-run hint: "`Tab` = model list, `Space` = next shape, `?` = help".
- Consistent key names between HUD and `--help` (`[up/down]` vs `Up / Down`,
  `POS1 (HOME)` vs `Pos1 / Home`).

## Model list (Tab panel)

- Filter/search by typing while the panel is open.
- Section headers: built-in shapes vs `models/`.
- Distinguish the *loaded* model from the *cursor*.
- Show metadata per entry: triangle count, LOD level.
- Mouse click to select/load (currently wheel + arrows only).
- Real scrollbar instead of only `^` / `v`; clearer truncation marker.
- Configurable / remembered panel width (currently fixed at `cols/4`).

## Colour sliders

- Hex/RGB readout in the label (`mesh #006400` instead of `mesh 128/256`).
- Faster navigation: `Page Up` / `Page Down` or jumps instead of one step per
  `Left` / `Right`.
- Presets/themes (e.g. "Renault green", "mono", "amber") and a reset.
- Direct access without opening the menu (e.g. `C` / `Shift+C`).
- Colour-blindness: the sliders rely on colour; add a position/hex readout.

## Feedback & progress

- Export progress (`frame 120/360`) and cancel with `ESC`; currently the result
  is only reported at the end.
- Loading indicator for large models (parsing + BVH build blocks briefly).
- Persistent LOD indicator instead of a 5-second message.

## Input

- Pause the rotation (dedicated key; `p` is taken by export).
- Reverse shape cycle (e.g. `Shift+Space`) and direct jump to shape 1-9.
- Mouse drag to rotate / tilt.
- `[` / `]` as alternatives to `+` / `-`.
- Shatter controls: gravity / reset, instead of only "`Enter` = rebuild".

## Rendering

- Ramp presets: alternative ASCII ramps / styles (blocks, dots, outline-only).
- Quality toggle (supersampling on/off) at runtime, without restarting.
- Fixed canvas / letterbox so the model stays put on terminal resize.

## Settings & persistence

- Remember shape, spin, tilt, mesh/light colour and FPS display between runs
  (e.g. `%LOCALAPPDATA%\ascii3D.ini` or the registry), plus `--no-config`.

## Polish

- Message severity (info / warning / error) and stacking instead of overwriting.
- CLI counterparts: `--hud`, `--theme`, `--list`.
- Set the console title to `ascii3D - <shape>`.
