# Butterscotch 3DS EPdN

Nintendo 3DS builds and compatibility work for Butterscotch, focused on running
GameMaker Studio Undertale and Deltarune data files on 3DS hardware.

This repository is based on the original open-source Butterscotch project:

- Original project: https://github.com/efimandreev0/Butterscotch
- Original author/repository owner: efimandreev0

The EPdN branch keeps the original project structure and adds 3DS-focused
runtime, renderer, launcher, diagnostic and Deltarune compatibility work.

No Undertale, Deltarune, `data.win`, executable, extracted texture, music,
sound, script, save, ROM, or other copyrighted game asset is included in this
repository or in the release packages. Users must provide their own legally
obtained game files on their own 3DS SD card.

## Nintendo 3DS Features

- Launcher for compatible GameMaker data files on 3DS.
- Installable CIA builds and Homebrew Launcher 3DSX builds where available.
- Deltarune and Undertale game profiles so Deltarune-specific fixes do not
  change Undertale behavior.
- Deltarune Chapter 2 rendering fixes for GMS2 layers, tilemaps, surfaces,
  battle UI and TP bar behavior.
- Deltarune chapter-save compatibility improvements.
- Debug Mode toggle for START/SELECT developer actions.
- Diagnostic dumps for hardware debugging.
- New 3DS C-Stick movement support.
- Shutdown, HOME-close and launcher-transition fixes.

## Installation

Install the CIA from a release, then place your legally obtained GameMaker game
files on the SD card using Butterscotch's expected 3DS directory layout.

The release QR code points directly to the CIA asset on GitHub and can be
scanned from FBI on a 3DS.

Audio requires the normal 3DS DSP firmware file:

```text
sdmc:/3ds/dspfirm.cdc
```

Luma3DS can create this file from the console's own firmware through Rosalina's
`Dump DSP firmware` command.

## Releases

The important EPdN releases are published here:

- `v2.9-EPdN`: maintainer-style base release with Undertale audio fixes,
  Deltarune startup/layer compatibility work, Spanish/Latin text fallback and
  cache controls.
- `v4.2-EPdN`: last version sent before the later Deltarune Chapter 2 repair
  work; important baseline for texture-cache and renderer behavior.
- `v7.2-EPdN`: latest package with the retained Deltarune Chapter 2 fixes,
  shared save compatibility, TP bar fix, grey-room fixes, Debug Mode, and the
  latest teacup/elevator native draw experiment.

Each GitHub release includes:

- installable CIA
- Homebrew Launcher 3DSX when available
- source snapshot zip
- QR code for FBI installation
- changelog and SHA-256 checksums

Latest release:

https://github.com/EstebanPdN/butterscotch-3ds-epdn/releases/latest

## Building

Requirements:

- devkitARM, libctru and 3ds-cmake under `DEVKITPRO`
- `makerom` and `bannertool` for CIA packaging

Typical build:

```sh
cmake -S . -B build-3ds -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake"
cmake --build build-3ds --target butterscotch_cia
cmake --build build-3ds --target butterscotch_3dsx
```

## Development Notes

`WORKFLOW.txt` records the EPdN version memory, including failed experiments and
the protocol used before creating new builds. New Deltarune-specific changes
should stay gated behind `GAME_PROFILE_DELTARUNE` unless there is a deliberate
cross-game reason to touch shared behavior.

## Legal

This repository contains source code, build scripts, documentation and release
artifacts for the open-source runner only. It does not distribute commercial
game assets.

Butterscotch is licensed under the license inherited from the original project.
See `LICENSE` for details.
