# The launcher (Windows)

`rac1-launcher.exe` is the easy way to get the game on Windows: one file to download and
double-click, with nothing to install first. It downloads the game, sets it up from your own disc
image, keeps it up to date, starts it, and manages your saves.

It is attached to each [release](https://github.com/daino-selvatico/rac1-native-game/releases) as
`rac1-launcher.exe`. It needs Windows 10 or 11 (it runs on .NET Framework 4.8, which both have).

> Windows may warn that the program is from an unknown publisher (SmartScreen: "Windows protected
> your PC"). The launcher is not signed: click **More info**, then **Run anyway**. Its source is
> `tools/launcher/` in this repository.

## Using it

1. Download `rac1-launcher.exe` and double-click it.
2. Click **Install** and choose your disc image (a plain `.iso` of the North American disc,
   SCUS-97199, or the European one, SCES-50916).
3. Wait. The first time takes a while (often 20 to 40 minutes): the launcher downloads the latest
   release, the tools that build it (about 1 GB), generates the game's code and data from your disc
   and builds the game. The progress shows on the **Play** page; **Show the details** shows the
   setup's own messages.
4. Click **Play**. The launcher puts a copy of itself in the game's folder and a shortcut in the
   Start menu (and, if you say yes, on the desktop): next time, start the game from there.

When a new release is out, the **Play** page says so: **Update** downloads it and rebuilds what
changed, and your saves and settings are kept. The launcher updates itself the same way.

## The pages

**Play**: Install, Set up (when the setup needs to run again, for example after a setup that did not
finish), Play, and Update. Cancel stops a running setup; the next run continues from where it stopped.

**Saves**: the save slots on the virtual memory card (see [Saves](saves.md)), with the time each was
last saved. **Back up all** puts the whole card in a zip; **Restore a backup** puts a zip back (the
saves as they are first go into `%APPDATA%\rac1-native\save-backups\`); **Delete the slot** moves a
slot to the Recycle Bin.

**Settings**:

- **Game folder**: where the game goes (by default `%LOCALAPPDATA%\rac1-native`, or
  `C:\rac1-native` when that path has spaces or non-ASCII letters). Choose a folder that already has
  the game (a clone, or a zip set up with `setup.cmd`) to use it from the launcher.
- **Disc image**: the `.iso` the setup uses.
- **Update channel**: **Stable** gets the releases; **Development** gets the newest changes on
  `main`, before they are released (new fixes sooner, and new bugs too). It is the same setting as
  the game's Options > Advanced > Update Channel (the `update_channel` line of
  `%APPDATA%\rac1-native\advanced_options.txt`): changing it in one changes it in the other. Install
  and Update follow it: on Development, Install downloads `main` as it is now, the **Play** page
  says when `main` has moved since, and Update gets it (`tools\setup.py --update --channel
  development`). Back on Stable, the next Update goes to the latest release.
- **Parallel jobs**: how many jobs the setup runs at once. Automatic is fine; 1 is the slowest and
  uses the least memory.
- **Set up again from scratch**: generates everything again from the disc and rebuilds (the setup's
  `--force`).
- **Open the setup's logs**: the `build\` folder, with `launcher.log` (everything the launcher ran
  and what it printed), `setup-build.log` and `setup-codegen.log`. Attach them to a bug report.

The game's other settings (graphics, controls) are in the game's Options menu.

## What it runs

The launcher does what `setup.cmd` does, without a console window:

1. `tools\windows\toolchain.ps1`: the private MSYS2 in `.toolchain\` (see
   [Setup](setup.md#on-windows-experimental)).
2. `tools\setup.py --no-play --iso <your image>` (with `--update` to update, `--force` to set up
   again from scratch), with `RAC1_LAUNCHER=1`: the setup then also prints `@step` and `@progress`
   lines, which the launcher shows as its progress bar.

All of it runs in a Windows job: Cancel, or closing the launcher, ends the whole tree of programs.
When the code generation fails because Python itself crashed (#72), the launcher runs the setup
once more with one job at a time.

A first install unpacks the release's source zip into the game folder and records it as
`tools\setup.py --update` would (`assets\update-version.txt`, and the files' hashes in
`assets\setup-state.json`), so later updates work the same as for a zip set up with `setup.cmd`.

The launcher's own settings are in `%APPDATA%\rac1-native\launcher.txt` (the game folder, the disc
image, the parallel jobs).

## Building it

```bash
dotnet build tools/launcher/Launcher.csproj -c Release -p:LauncherVersion=0.4.1
```

Any .NET SDK 6 or newer builds it; the result, `rac1-launcher.exe`, needs only .NET Framework 4.8.
The [launcher workflow](../.github/workflows/launcher.yml) builds it at each push and attaches it to
each published release, with the release's version (`project(... VERSION)` in `CMakeLists.txt`).
A launcher built without a version (0.0.0) never updates itself.
