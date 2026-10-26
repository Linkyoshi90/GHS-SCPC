# GHS:SCPC

An unofficial native **Windows PC port** of the PlayStation 2 horror adventure **GHS: Soul Collector** (European
release, serial **SLES-51933**, v1.00), built by *static recompilation*: the game's own PS2 program is translated into
C++ and compiled into a Windows program, then run on a runtime that stands in for the PS2's hardware.

**This repository contains no game files.** No disc image, no game program, no graphics, sound or text from the game,
and no code generated from it. You need **your own copy of the game** (a disc image you dumped yourself). The launcher
reads your disc image, and the build turns it into the playable port on your own computer.

> **Made mainly by AI.** Nearly all of this port's code, reverse engineering, tooling and documentation was written by
> an AI model (Anthropic's Claude, working through Claude Code), directed, play-tested and reviewed by the repository's
> owner. Expect the rough edges of an AI-built hobby project; bug reports are welcome.

Not affiliated with or endorsed by the game's publisher or rights holders. All trademarks belong to their owners.

---

## Contents

- [What you need](#what-you-need)
- [Installation and first start](#installation-and-first-start)
- [Playing](#playing)
- [Features](#features)
- [Optional: Japanese voices](#optional-japanese-voices)
- [Optional: online Hell's Roulette](#optional-online-hells-roulette)
- [Where things are](#where-things-are)
- [Updating](#updating)
- [Troubleshooting](#troubleshooting)
- [How it works](#how-it-works)
- [For developers](#for-developers)
- [Credits](#credits)

---

## What you need

| | |
|---|---|
| **System** | Windows 10 or 11, 64-bit. A DirectX 11 graphics card (any from the last decade). |
| **The game** | Your own dump of the **European disc, SLES-51933** (v1.00), as a plain **`.iso`** file. Other formats (`.bin/.cue`, `.chd`, `.cso`) and other releases (US, Japan) are not supported as the main game. The launcher checks the version. |
| **Git** | https://git-scm.com/download/win (default options are fine). |
| **Visual Studio** | Visual Studio with the workload **"Desktop development with C++"** (the free Community edition is fine; built and tested with Visual Studio 2026). https://visualstudio.microsoft.com/ |
| **Internet** | The build downloads the recompiler (PS2Recomp) and libraries (raylib, Dear ImGui, FFmpeg). |
| **Disk space** | About **15 GB** free: the unpacked game (3 GB), the recompiler and its build (7 GB), the generated code (0.3 GB), plus room for logs and saves. Visual Studio itself is not counted. |
| **Time** | The one-time build takes **30 to 60 minutes**, depending on your CPU. |
| *Optional:* **Python 3** | For the cheat menu's item names, and with **numpy** (`py -m pip install numpy`) for Japanese voices. https://www.python.org/downloads/ |

## Installation and first start

1. **Get the repository**: clone it (`git clone <this repository's URL>`) or download it as a ZIP and extract it.
   Use a path without special characters, for example `C:\Games\GHS-SCPC`.
2. **Start `GHS-SCPC.exe`** (the launcher) in that folder. Windows SmartScreen may warn about an unknown program:
   *More info → Run anyway* (or build the launcher yourself, see [For developers](#for-developers)).
3. Follow the three steps the launcher shows:
   1. **Unpack your disc**: press *Choose disc image...* and pick your `.iso`. The launcher reads it directly (no
      mounting), checks that it is the right release and copies the game files into the `game` folder. Your disc
      image is not changed.
   2. **Build the game**: starts by itself after unpacking. The log shows what happens: the recompiler is downloaded
      and patched, your game's program is recompiled to C++, and the port is compiled. **30 to 60 minutes, one time.**
   3. **Play**: press *Play*.
4. **First session**: some of the game's graphics programs (VU1 microcode) still run on a slower interpreter. They are
   saved while you play; the next time you press *Play*, the launcher translates them once (a few minutes), and the
   game runs at full speed from then on. If it feels slow on your first start, that is expected.

The launcher can play background music from the game once the disc is unpacked (*Music* button, *Next track*).

## Playing

Default keys (change them in **F2 → Controls**, or press **F4**). XInput controllers (Xbox and compatible) work
out of the box, including vibration.

| Action | Keys |
|---|---|
| Run / walk | **WASD** (hold **C** to walk, **Left Ctrl** to walk slowly) |
| D-pad (slow walk, menus) | Arrow keys |
| Cross / Circle / Square / Triangle | **X** or **Space** / **C** or **Esc** / **Z** / **V** |
| L1 / R1, L2 / R2 | **Q** / **E**, **Left Shift** / **Right Shift** |
| Start / Select | **Enter** / **Tab** |
| Settings | **F2** (resolution, widescreen, smoothing, controls, Japanese voices) |
| Key bindings | **F4** |
| Cheats | **F5** |
| Snapshot (RAM + screenshot, for bug reports) | **F6** |
| Frame counter | **F3** |
| Full screen | **F11** |
| Fast-forward | **Backspace** (hold) |

Saves go to the virtual memory card in `recomp\memcards` (save at the frog in the game, as usual).

## Features

Besides playing the game natively:

- **Graphics**: the PS2's graphics chip runs on your GPU (Direct3D 11), with **render scale 1x to 4x**, **16:9
  widescreen**, smooth (interpolated) character animation, smoother shadows at higher scales, sharper scaled icons.
- **Speed**: the original game speed in both of the game's display modes (50 Hz, and 60 Hz, where the port corrects
  the parts that would otherwise run 1.2x too fast); Backspace fast-forwards.
- **Controls**: rebindable keys, controllers with vibration, **mouse look** in first-person view and when peeping
  through keyholes.
- **Cheats (F5)**: infinite mental gauge, infinite items, invulnerability, no detection (guests don't see, hear or
  notice you; equipping a story item that a guest has to react to, such as the screwdriver, switches it off),
  permanent Speed Up, no conditions, in-game clock speed, Roulette spin results, give/remove items, a **guest radar**
  overlay, and **warp** to any room.
- **Item randomizer (F5)**: shuffles where items lie, optionally moves them to completely new spots in other rooms,
  and can shuffle which rooms the guests visit each night (their own rooms stay, so every soul can still be taken).
  Saving at the frog keeps the shuffle.
- **Japanese voices** (optional, see below) with the English game.
- **Online Hell's Roulette** for two players (optional, see below).

## Optional: Japanese voices

If you also own the **Japanese release** (SLPM-65324), you can hear its voice recordings with the English text:

1. Install Python 3 and numpy (`py -m pip install numpy` in a command prompt).
2. In the launcher (after unpacking the European disc), press **Japanese voices...** and pick the Japanese `.iso`.
   It copies the Japanese voice data next to your game files and matches each English line to its Japanese
   recording (a few minutes).
3. In the game: **F2 → Japanese voices**.

## Optional: online Hell's Roulette

Two players can play the roulette board game over the network. One player runs the small room server
(`python tools\roulette_server.py`, TCP port 7790, open it in the firewall), both start the game with
`powershell -ExecutionPolicy Bypass -File tools\launch.ps1 -Online <server>:7790 -Room <name>` and pick Hell's
Roulette for two players on the title screen. The first player in a room plays blue, the second red.

## Where things are

| Path | What |
|---|---|
| `GHS-SCPC.exe` | The launcher (source: `tools\launcher.cs`). |
| `Play GHS-SCPC.cmd` | Starts the built game directly, without the launcher (runs `tools\launch.ps1`). |
| `game\` | Your unpacked game files *(made by the launcher; never share them)*. |
| `PS2Recomp\` | The recompiler and runtime, cloned and patched by the build; the port itself is `PS2Recomp\out\build\ps2xRuntime\Release\ps2EntryRunner.exe`. |
| `recomp\memcards\` | Your saves. |
| `recomp\live.log`, `recomp\live.err.log` | The last session's logs (the one before is kept as `live.prev.*`). |
| `recomp\recomp.log`, `recomp\runner-build.log` | Build logs. |
| `overrides\` | The port's own game fixes and features (C++), compiled into the game. |
| `patches\ps2recomp-local.patch` | All changes to the PS2Recomp runtime, applied to a pinned upstream commit. |
| `recomp\ghs.toml`, `recomp\ghs_functions_code.csv` | Recompiler settings and the game's function list (addresses and names only). |
| `tools\` | Build, launch and helper scripts. |

## Updating

After pulling a new version (`git pull`, or a new ZIP extracted over the old folder, keeping `game\` and `recomp\`):

- If only `overrides\` or `tools\` changed: run `powershell -ExecutionPolicy Bypass -File tools\install_overrides.ps1`,
  then rebuild with `tools\setup.ps1` (or delete `PS2Recomp\out\build\ps2xRuntime\Release\ps2EntryRunner.exe` and
  start the launcher, which then offers *Build the game*).
- If `patches\ps2recomp-local.patch` changed: delete the whole `PS2Recomp` folder and start the launcher; it builds
  everything again (30-60 minutes). Your saves in `recomp\memcards` are kept.

## Troubleshooting

| Problem | What to do |
|---|---|
| "This disc boots ..., not the European release" | Only the European v1.00 disc (SLES-51933) works. Check your dump against redump.org. |
| "Not a DVD image with an ISO 9660 file system" | Use a plain `.iso` dump, not `.bin/.cue`, `.chd` or `.cso`. |
| "Git is missing" / "Visual Studio ... is missing" | Install them (see [What you need](#what-you-need)), then press the button again. |
| The build stops with an error | Read the last lines of the launcher's log and `recomp\runner-build.log`. Pressing the button again continues where it stopped. |
| The game is slow on the first start | Expected: see step 4 of [Installation](#installation-and-first-start). |
| The game does not start | Look at `recomp\live.err.log`. |
| Something looks or behaves wrong | Press **F6** at that moment (saves a snapshot to `game\snapshots`) and open an issue with a description and `recomp\live.err.log`. Don't attach files from `game\`. |

Linux (Wine/Proton): the launcher can start an already built port without PowerShell, but building needs Windows with
Visual Studio.

## How it works

1. **Recompiler inputs** (in this repo): the game's function list (`recomp\ghs_functions_code.csv`, addresses and
   names found with Ghidra) and the recompiler settings (`recomp\ghs.toml`). No game code is stored here.
2. **PS2Recomp** ([ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp)) is cloned at a pinned commit and
   `patches\ps2recomp-local.patch` is applied: the runtime changes this port needs (graphics on the GPU, the VU1
   translator, audio, memory cards, controllers, the settings and cheat windows, and many fixes).
3. The recompiler reads **your** `game\SLES_519.33` and writes about 35,000 C++ files (in `recomp\output`, then
   copied into `PS2Recomp`), together with the files from `overrides\`: the port's game-specific fixes and features.
4. Everything is compiled into `ps2EntryRunner.exe`, which runs the recompiled game and reads the rest of the game data
   (`FILE.STM`, sound, movies) from `game\` at run time.
5. VU1 microcode the game uploads is translated to C++ the first time it is seen (`tools\add_vu1_images.ps1`, run by the
   launcher), because it is game code and can't ship with the repo either.

## For developers

- **Launcher**: build it with the C# compiler that comes with Windows (from the repository folder):
  `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /target:winexe /win32icon:ghs.ico /out:GHS-SCPC.exe tools\launcher.cs`
- **Overrides**: edit `overrides\*.cpp`, then `tools\install_overrides.ps1` and rebuild (only the changed files compile).
- **Runtime changes** in `PS2Recomp\`: afterwards run `tools\save_ps2recomp_patch.ps1` so `patches\ps2recomp-local.patch`
  contains them (the generated game code is excluded automatically).
- **Debug options** of `tools\launch.ps1`: `-Cpu` (graphics on the CPU), `-Scale N`, `-Pcm` (record sound), `-TexCheck`,
  `-Record` (GS recording with `tools\gs_record_now.ps1`), `-Capture` (record VU1 programs).
- Never commit anything from `game\`, `PS2Recomp\` or the generated parts of `recomp\` (the `.gitignore` keeps them out).

## Credits

- **PS2Recomp** by ran-j and contributors: the static recompiler and runtime this port is built on.
- **raylib**, **Dear ImGui** and **FFmpeg**, downloaded by the build.
- **Ghidra** and the **ghidra-emotionengine-reloaded** extension, used to find the game's functions.
- **PCSX2**, used as the reference while fixing the port.
- The port itself: written mainly by **Claude** (Anthropic), directed and tested by the repository owner.
