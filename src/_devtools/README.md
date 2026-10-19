# `src/_devtools/` — openly invented development tools

Per [ADR 0003](../../docs/decisions.md), this directory holds code that is **openly invented** to
support development:

- A dev menu inspired by R&C1's hidden Insomniac debug menu
- Spawn / kill / teleport helpers
- Render-state toggles (wireframe, no-cull, depth-test off)
- Force-state controls (skip cutscene, give gadget, set checkpoint)
- Visual-debug overlays (collision wireframes, bounding boxes, etc.)

## Policy

- **Openly invented**: there is no `// orig:` requirement here. This code is *not* a port; do not
  pretend it is.
- **Compiled only when** the `RAC1_DEVTOOLS` CMake option is ON (the default). A build with
  `RAC1_DEVTOOLS=OFF` leaves this whole tree out.
- **No header from here may be `#include`d by** `src/game/` or `runtime/`. Only `src/host/` and
  other files in `src/_devtools/` may include them.
- **Uses ImGui freely**: that is the point.
- May read and write EE RAM and call the hardware layer in `runtime/`. It should not call functions
  in `src/game/` that are not meant to be called from outside.

## The lighting's editors

- **Lights** (tab): the modern lighting's lights placed by hand and the automatic emitters' classes
  turned off, per level, saved in `data/lights/levelNN.txt` (see
  [Lighting: the light editor](../../docs/lighting.md#the-light-editor)). Its "Lights per object class" part picks the object
  under a crosshair (the depth there: a tie instance's box, the nearest moby) and makes every instance of its class emit
  (`emit` lines; see [Lighting: lights per object class](../../docs/lighting.md#lights-per-object-class)).
- **Sun** (tab): the level's own sun direction (azimuth and elevation, or the game's key light),
  applied at once while playing; Save writes the `sun` line into the same level file, keeping its
  comments and lights (see [Lighting: the sun editor](../../docs/lighting.md#the-sun-editor)).
- **Sun** (tab), interiors: the level's `interior` boxes (the camera inside: the sun off, the original's look) drawn over the
  picture, added around the camera, edited, deleted; `interior_set` light sets (the hero lit with one); saved with the level file
  (see [Lighting: interiors](../../docs/lighting.md#interiors)).
