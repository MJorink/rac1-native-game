# Changelog

What changed in each release. Releases are source only: you build the game with `./setup.sh`
(`setup.cmd` on Windows) from your own disc. Issue numbers refer to this repository's issues.

## Unreleased

### Windows
- **The launcher**, `rac1-launcher.exe`: one file to download and double-click, attached to each
  release. It downloads the game, sets it up from your disc image, updates it (and itself), starts
  it, and backs up, restores and deletes saves. See [The launcher](docs/launcher.md).
- The setup no longer fails downloading GLFW with "error adding trust anchors" when the game's
  folder has a space in its path (#79, #69, #63): MSYS2's certificate files, left empty by its
  CLANG64 tool there, are made again by `tools\windows\toolchain.ps1` (also on a setup made before).
- A code generation job whose Python crashes ("Fatal Python error", #72) is run once more on its own.
- A clearer message when the program disappears after the build (an antivirus removing it, #69).

## 0.4.0

Bug fixes from the 0.3.0 reports, and almost all of the game's code is now ported: of the code the
game runs, one function is left (see the [port inventory](docs/port-inventory.md)). The whole game
has been played through to the end on it, and the start of a New Game+ checked; tested on Linux only
so far.

### Crashes
- **Quartu:** the crash under water when the air meter runs low (very likely #48's at the Box
  Breaker path): the game asks for a sound that does not exist, which the PS2 ignores.
- **Kalebo III:** the crash during the landing scene in widescreen with a raised level of detail.

### Gameplay
- **Modern aiming** aims up and down with every weapon that fires in a direction (Devastator,
  R.Y.N.O., Visibomb, Pyrocitor, Tesla Claw, Morph-o-Ray, Suck Cannon), and the gloves' throws land
  nearer or farther with the aim (#48).
- **RAC 3 movement:** jumping during a standing wrench throw (#50).
- The camera no longer locks for a second after a strafe (#56, thanks to MJorink).
- Giant Clank's triangle attack no longer leaves streaks behind (#51); the laser beams on Drek's
  fleet move instead of hanging frozen (#53). Every particle type of the game is now ported.
- Kerwan's hanging cables are caught as in the original (Kerwan's own rail search).
- The Metal Detector's display now shows on every planet, not only Batalia and Drek's fleet.
- **Quit Game** now goes back to the title screen and its main menu, as the original, instead of
  closing the game.
- A cutscene started while strafing no longer keeps Ratchet in the strafe pose (#40).
- The pause menu's **Goodies** pages after finishing the game: Cheats, In-Level Movies, the
  Sketchbook, the Epilogue and the Skill Points text.

### Graphics
- **Widescreen (16:9, 32:9):** the water, lava and sand sea are no longer cut at the 4:3 sides (#6);
  thin far objects (Gaspar's antennas and spires, far plants) are no longer missing at a raised
  level of detail or in widescreen.
- Force fields, Quartu's Hologuise gates and Orxon's O2 barriers are see-through and tinted instead
  of black (#52).
- The Vendor's screens on Aridia, Eudora, Umbris and Oltanis (the Tesla Claw page) (#47, #48).
- Hoven: the ice cube around Ratchet is whole again (#48).
- A high Field of View no longer clips the sky (a black band at the top, Pokitaru's sky after
  surfacing), and the ship menu's planets are no longer drawn as squares.
- The starry sky in the loading screen's flight after Load Game from the title, with the lighting on.
- **Modern lighting** on Batalia to the Veldin finale: rooms and corridors no longer go dark under
  the roof's shadow; false lamps on Gaspar and Orxon removed.
- A **Monitor** row on the Graphics page, to choose the screen for fullscreen (#44, thanks to CDCast).

### Windows and the updater
- **Update now** without git: a clone that git cannot update is updated from the source zip,
  keeping your own files (#47). (Players on 0.3.0 without git need to update by hand once.)
- Windows: a click in the console no longer freezes the game (QuickEdit off), the frame timing is
  more precise, and alt-tab from fullscreen / a minimized window no longer breaks the picture (#41).
- Window sizes down to 640x480 (#46).

## 0.3.0

**Every planet of the game is ported**, up to the final fight with Drek, the credits and the end
menu. The planets after Orxon are new and **not fully tested yet**: expect bugs and glitches; some
polishing work is still to do. Tested on Linux only so far.

### New planets
- **Pokitaru** (finished), **Hoven**, **Gemlik Base** (Qwark's space fight), **Oltanis**, **Quartu**
  (Giant Clank), **Kalebo III** (Gadgetron's headquarters, the hoverboard race), **Drek's fleet** and
  **the Veldin finale**, each with its own build of the hero's code, its classes, effects, cameras
  and sounds, ported from the game's code and proved against the original.
- **The end of the game:** the credits, the last movie and the end menu (a new game keeping your
  weapons and bolts, or back to before the fight with Drek).
- The PDA now opens the Vendor on every level.

### Performance
- Faster vector math (SSE2), a faster dispatch of the game's callbacks and lighter renderer passes;
  the experimental lighting is faster too. The game's own state is unchanged.
- The levels are read straight from the disc image: the setup no longer copies them (335 MB less).

### The updater
- **Update now** from the update notice: the game closes, updates its sources (git, or the release's
  source zip), runs the setup again and restarts. Source only, as always; local changes are never
  overwritten (#13).
- A dedicated **Updates** page on the title's main menu: the check on / off, the channel (Stable /
  Development) and **Check now**.

### Also new
- **Strafing movement** (Advanced): R&C 1's movement with the strafe and the strafe flips (#34).

### Fixes
- Batalia's cutscenes out of sync: the pause menu now pauses the dialogue and music as on the PS2 (#23).
- Particles that froze in place on several planets (smoke, debris, effects), as the helicopter
  bullets did before.
- Kalebo III's race HUD showed its text in the wrong colour.
- Experimental lighting: the sun's shadow no longer flips when entering buildings (#3).
- The black bars of the 4:3 view were missing (#39).

## 0.2.1

Windows fixes, confirmed by a player on a laptop with an NVIDIA GPU (#35):
- On laptops with two GPUs, the game now starts on the dedicated one (NVIDIA or AMD) instead of the
  integrated one. A choice made in Windows' settings or in the driver's panel still wins.
- The flickering (the title's scene in a wrong view, the menus' frames disappearing) is gone: a
  frame that came sooner than the game's next one was drawn again with the wide-screen projection
  applied twice.
- `play.cmd` checks that the setup is up to date and, after a new version or an unfinished setup,
  offers to run it again. The setup no longer redoes everything when run from another Windows shell.

## 0.2.0

### New
- **Batalia, Gaspar and Orxon** are ported and playable: Batalia's rain, its turret and the Trespasser
  puzzle; Gaspar's lava and the Pilot's Helmet (it could not be picked up before, on any level);
  Orxon's toxic goo, Clank's section and Ratchet's part with the O2 mask.
- Weapons and gadgets shared by every level: the Drone Device's drones, the Morph-o-Ray's chickens
  and its HUD element.
- **The European PAL disc** (`SCES-50916`) works on its own, without the North American disc (#12). It
  runs in the PAL game's own 60 Hz mode. Ten functions that the PAL release changed are still
  temporary versions, not yet proved against the original (see the Setup page).
- **Windows support, experimental** (#11): the same sources build on Windows; `setup.cmd` downloads
  everything it needs, and `play.cmd` starts the game.
- **An update check** (#14): at start the game checks GitHub for a new release and says so at the main
  menu. It downloads nothing, and can be turned off in Options > Advanced. A work in progress.
- **The oracle is in this repository** (`tests/oracle`, [Oracle](docs/oracle.md)): the tests that prove
  each port against the original code.

### Fixed
- Controllers missing from the controller database (for example the GameSir G7 SE) were ignored: on
  Linux, Xbox-style controllers now get the Xbox layout automatically (#1).
- The language chosen in the game is kept for the next start, as a PS2 keeps its system language (#15).
- Below 60 fps (the 30 FPS cap, V-Sync at 75 Hz, a heavy scene) the game lost frames: it ran slow and
  the movies' sound fluttered (#4, #22; better on #8 and #24).
- Effects turned black or invisible after a while on Aridia on NVIDIA cards (glows, smoke, bolts, the
  sand), which could then crash the game (#19; to be confirmed on NVIDIA).
- The air meter left a line at the screen's edge after diving (#5).
- The Vendor showed a chicken icon and 0/0 ammo for the centre weapon (#27).
- Kerwan's helicopters' bullets froze in mid-air (#26).
- Advanced options changed at the main menu were lost if you quit without playing.
- On Windows, the setup failed with a disc image whose path contains `&` (#35).

## 0.1.0

Milestone 1: the game playable from Veldin to Umbris, on Linux, from the North American disc.
