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

## The Unlocks tab (`dev_unlocks.cpp`)

What the game keeps of what was found and bought, given and taken back through the game's own state: the same bytes and
writes as the game's code, so the pause menu, the Vendor, the cases and a save see them as the game's own (a save made
after giving loads back with them). Only in the debug menu (`--debug-tools`); testing: `RAC1_UNLOCKS` (docs/env-vars.md).

- **Items** (the pause menu's Gadgets and Items pages): the gadgets (Swingshot, Hydrodisplacer, Trespasser, Metal
  Detector, Hologuise, PDA), the packs, the helmets (Sonic Summoner, O2 Mask, Pilot's Helmet), the boots, the
  Hoverboard, the Map-o-matic, the Bolt Grabber, the Persuader: the items owned `0x13d4c0[id]`, given by the game's
  `item_give` (0x260860; owned, "seen" `0x13d4e8`, ammo, the level gadget's quick-select place). Taken back: owned and seen
  cleared, out of the quick select (`0x141ea0`, `0x15edd0`), a worn item taken off (the slot's request `0x141408[kind]` =
  0x26, as the pause menu's closing does), a held one replaced by the wrench. The goodies of the Items page are the bytes
  `0x13d388[i]`: Zoomerator 0, Raritanium 1, CodeBot 2, Premium Nanotech 4, Ultra Nanotech 5; the nanotech also sets the
  health maximum `0x15eda0` (5 / 8, back to 4 without them) and fills the health, as Orxon's vendor does.
- **Gold bolts**: `0x14bec0[level * 4 + k]`, one byte a bolt (the Gold Bolt moby 1134 sets its pvar +0's byte); 40 in all,
  per planet: Novalis 3, Aridia 4, Kerwan 3, Eudora 1, Rilgar 2, Nebula G34 2, Umbris 2, Batalia 2, Gaspar 2, Orxon 2,
  Pokitaru 1, Hoven 2, Oltanis Orbit 1, Oltanis 4, Quartu 2, Kalebo III 2, Veldin Orbit 2, Veldin (finale) 3 (counted on
  every level). The pause menu's Items page shows found (at most 40), used (4 per gold weapon) and left. The bolts of the
  planet you are on appear / disappear when it is loaded again.
- **Gold weapons**: `0x13e520[item]`, as Novalis's gold weapon cases (class 304) set it: Tesla Claw, Bomb Glove,
  Devastator, Pyrocitor, Mine Glove, Blaster, Morph-o-ray, Glove of Doom, Decoy Glove, Suck Cannon (the cases' table). The
  menus' icons and names, the quick select, the Vendor's models and the weapon in hand turn gold at once. "All" also
  gives the weapons not owned (the cases sell a gold weapon only for one owned); "None" leaves the weapons.
