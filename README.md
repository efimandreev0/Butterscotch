<h1 align="center">🥧 Butterscotch 🥧</h1>

<!-- Badges, about the GitHub repository itself -->
<p align="center">
<a href="https://discord.gg/2gQR7t3WJR"><img src="https://img.shields.io/discord/1406856655920168971?color=5865F2&logo=discord&logoColor=white&label=discord"></a>
</p>

## Running on Nintendo 3DS

First, install the provided `.cia` file on your console (using FBI or your preferred installer).

Next, you'll need to copy your game files to your SD card into the following directory:
`sdmc:/3ds/butterscotch/undertale/`

> **Note:** If you want the game's icon to actually show up in the launcher, just drop the original game's `.exe` file into that same folder alongside the other files. The runner will automatically extract the icon from it!

### Launcher features

The 3DS build ships with a built-in launcher that scans `sdmc:/3ds/butterscotch` for installed games and lets you tweak how they run. Press **Select** on the grid to open settings, **L + R + A** while in-game to bring up the pause menu.

* **Themes** — 5 colour palettes that retint the whole launcher UI (background gradient, particles, accents, text). Choices: `BUTTERSCOTCH`, `MIDNIGHT`, `FOREST`, `ROSE`, `CARBON`.
* **Game screen** — render the game on the **top** or **bottom** screen. The other screen automatically shows the same backdrop / letterbox.
* **Empty space (backdrop mode)** — what fills the screen around a non-fullscreen game:
  * `GRADIENT` — themed gradient.
  * `BLUR` — blurred, scaled-up copy of the game itself (looks great on rooms with a strong palette).
  * `BLACK` — plain black bars.
  * `STRETCH` — no letterbox, the game is stretched to fit (breaks aspect ratio).
* **Side particles / Side blur** — toggle the animated particles and blur effect drawn on top of the backdrop.
* **OS type** — overrides the value GMS sees from `os_type`. Some games branch on this (e.g. Undertale's Xbox edition needs `xboxone`); leave on `WINDOWS` for the default Steam build.
* **Input mode** — `KEYBOARD` / `GAMEPAD` / `TOUCH`. Picks which device the runtime exposes to the game; useful for games that hard-code one input style.
* **Global controls** — remap the buttons used by every game (D-Pad, A/B/X/Y, L/R, Start, Select).
* **Per-game controls** — open the pause menu while the game is running to remap controls for that game only, without touching the global mapping.
* **Settings persistence** — everything saves to `sdmc:/3ds/butterscotch/launcher_settings.bin` and is reloaded on next boot. Press **Y** in the settings menu to restore defaults.

### Preprocessing (`ctr-cache-preprocess`)
For the game to run on the 3DS hardware, the files need to be preprocessed. You have two ways to do this:

1. **Preprocess on your PC (Recommended):** Use the `ctr-cache-preprocess` tool included in the release to process the game files *before* moving them to your SD card. This is way faster. You can also build it from source — see [Building Butterscotch](#building-butterscotch).
2. **Preprocess on the 3DS at first boot (Slow):** If you just put the raw, unprocessed game files on your SD card, the console will handle the preprocessing itself the first time you launch the game. **Be patient!** This takes a really long time, so don't panic if it looks stuck—just let it do its thing.

### Current Playability Status
Here is what we've tested on actual 3DS hardware so far:
* **Undertale:** Fully playable. A complete True Pacifist route has been tested and beaten from start to finish.
* **Deltarune:** Chapters 1, 2, and 3 have been successfully tested to boot. Chapter 4 also booting, but isn't working.
* **WARNING:** Mods can don't work on Butterscotch-3DS.

> [!IMPORTANT]  
> Butterscotch is still VERY early in development and it is NOT that good yet.

When you create a game in GameMaker: Studio and export it, GameMaker: Studio exports the game code as bytecode instead of native compiled code, and that bytecode is compatible with any other GameMaker: Studio runner (also known as YoYo runner), as long as they have matching GameMaker: Studio versions. This is similar to how Java applications work.

This is how projects such as [Droidtale](https://mrpowergamerbr.com/projects/droidtale) (which was also made by yours truly) can exist. We exploit that GameMaker: Studio games compile to bytecode, which means they can be ran on *any* platform that has an official runner for it!

Ever since I created Droidtale 10+ years ago, I had that lingering thought in my mind... If GameMaker games use bytecode, what prevents us from creating our *own* runner? And if we can write our *own* runner, what prevents us from porting GameMaker: Studio games to other platforms?

And that's where Butterscotch comes in! Butterscotch is an open source re-implementation of GameMaker: Studio's runner.

**Butterscotch PlayStation 2 ISO Generator:** https://butterscotch.mrpowergamerbr.com/

## Game Compatibility

Butterscotch's goal is to be able to have Undertale v1.08 (GameMaker: Studio 1.4.1804, Bytecode Version 16) fully playable. But we do want to support more GameMaker: Studio games in the future too!

While our target is Undertale v1.08, that doesn't mean that other games CAN'T run in Butterscotch! Because Butterscotch is a runner and not a Undertale port/remake, you CAN run other GameMaker: Studio games with it and, as long as the game is compiled with GameMaker: Studio 1.4.1804 and they only use GML variables and functions that Butterscotch supports, it should work fine.

Here are the Bytecode Versions that Butterscotch supports

* Bytecode Version 16
* Bytecode Version 17

However, that doesn't mean that a game that uses a compatible version WILL run! The bytecode support is still a WIP, and Butterscotch may have quirks that the original GameMaker: Studio runner may not have.

Of course, there are exceptions that break game compatibility altogether:

* Games compiled with YYC, because they use native code instead of bytecode.
* Games compiled with the new [GMRT](https://github.com/YoYoGames/GMRT-Beta/tree/main), because they use native code instead of bytecode.

## Supported Platforms

* Linux (GLFW, OpenGL)
* macOS (GLFW, OpenGL)
* Windows (GLFW, OpenGL, MinGW)
* PlayStation 2 (ps2sdk, gsKit)
* Nintendo 3DS (devkitPro, citro3d)
* Haiku (GLFW)
* ...and maybe more in the future!

## Community Ports

* [Xbox 360 (Butterscotch-360)](https://github.com/ceilingtilefan/Butterscotch-360) by @ceilingtilefan
* [3DS and Wii U (Cinnamon)](https://github.com/Project-Sunshine-Native/cinnamon) by @casrielasriel, @grayforz24682, @d16.dorian, @ralcactus

## Building Butterscotch

```bash
mkdir build && cd build
cmake -DPLATFORM=glfw -DCMAKE_BUILD_TYPE=Debug ..
make
```

If you are using CLion, set the platform in `Settings` > `Build, Execution, Deployment` > `CMake` and add `-DPLATFORM=glfw`

Then run Butterscotch with `./butterscotch /path/to/data.win`!

### Other targets

* **Nintendo 3DS:** requires [devkitPro](https://devkitpro.org/) with the `3ds-dev` group installed. With `DEVKITPRO` exported, run `cmake -DPLATFORM=3ds ..` and `make` — the build produces a `.3dsx`/`.cia` you can install via FBI.
* **PlayStation 2:** `cmake -DPLATFORM=ps2 ..` (needs `ps2sdk` and `gsKit`).
* **`ctr-cache-preprocess` (PC tool):** `cmake -DPLATFORM=ctr-cache-tool ..` builds the standalone preprocessor used to bake textures for the 3DS target before copying the game to your SD card.

## CLI parameters

The GLFW target has a lot of nifty CLI parameters that you can use to trace and debug games running on it.

* `--debug`: Enables debugging hotkeys
* `--screenshot=file_%d.png`: Screenshots the runner, requires `--screenshot-at-frame`.
* `--screenshot-at-frame=Frame`: Screenshots the runner at a specific frame. Can be used multiple times.
* `--headless`: Runs the runner in headless mode. When running in headless mode, the game will run at the max speed that your system can handle.
* `--print-rooms`: Prints all the rooms in the `data.win` file and exits.
* `--print-declared-functions`: Prints all the declared functions (scripts, object events, etc) in the `data.win` file and exists.
* `--trace-variable-reads`: Traces variable reads
* `--trace-variable-writes`: Traces variable writes
* `--trace-function-calls`: Traces function calls
* `--trace-alarms`: Traces alarms
* `--trace-instance-lifecycles`: Traces instance creations and deletions
* `--trace-events`: Traces events
* `--trace-event-inherited`: Traces event inherited calls
* `--trace-tiles`: Traces drawn tiles
* `--trace-opcodes`: Traces opcodes
* `--trace-stack`: Traces stack
* `--trace-frames`: Logs when a frame starts and when a frame ends, including how much time it took to process each frame.
* `--always-log-unknown-functions`: When enabled, Butterscotch will always log unknown functions instead of logging them once per script.
* `--always-log-stubbed-functions`: When enabled, Butterscotch will always log stubbed functions instead of logging them once per script.
* `--trace-bytecode-after-frame`: When set, controls when `--trace-opcodes` and `--trace-stack` will start logging. Useful when debugging interpreter-heavy scripts.
* `--exit-at-frame=Frame`: Automatically exit the runner after X frames.
* `--speed`: Speed multiplier
* `--seed=Seed`: Sets a fixed seed for the runner, useful for reproduceable runs.
* `--print-rooms`: Prints all rooms to the console, along with all objects present in the room.
* `--print-declared-functions`: Prints all declared GML scripts by the game
* `--disassemble`: Dissassembles a specific script
* `--record-inputs`: Records user inputs
* `--playback-inputs`: Playbacks user inputs
* `--os-type`: Allows changing the built-in `os_type` value. The default is Windows. Example: When running Undertale Xbox, you would need to set it to `--os-type xboxone`.
* `--profile-gml-scripts`: Logs which GML scripts are the heaviest in terms of time and executed instructions.
* `--profile-opcodes`: Ranks which GML opcodes were executed the most.

## Debug Features

When running Butterscotch with `--debug`, the following hotkeys are enabled:

* `Page Up`: Moves forward one room
* `Page Down`: Moves backwards one room
* `P`: Pauses the game
* `O`: While paused, advances the game loop by one frame
* `F12`: Dumps the current runner state to the console
* `F11`: Dumps the current runner state to the console (JSON format), or dumps it to a file if `--dump-frame-json-file` is set.
* `F10`: Sets the `global.interact` flag to `0`. Useful in Undertale when you are moving through rooms and one of them starts a cutscene that doesn't let you move.

## Performance

Performance is pretty good on any modern computer, but when running on low end targets (like the PS2) it is *very* slow when there's a lot of instances on screen, or when a instance does a for loop.

## Then why not have a transpiler?

The issue with a transpiler is that, if you try transpiling the game in the "naive" way, that is, emitting VM calls like it was the original bytecode, you won't get any
*improvement* from it, you would need to create a *good* transpiler that actually transpiles it into *good* code, and that's way harder.

Having a transpiler also have other disadvantages:

1. You lose the ability of debugging the runner at a "high level" by tracing opcodes.
2. Compilation is SLOW, transpiling Undertale in a naive way to C and building it takes 90 seconds on a modern computer, and building it to other targets is so slow that I wasn't even able to test it.

## Screenshots

### Undertale (GLFW) [Bytecode Version 16]

<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/6651cc2e-0d6d-4354-b98d-081e84a981df" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/1d6edc51-2829-4f8f-b900-393f21a6655b" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/0d41f16c-7ee5-47de-a2e8-5831cdcd2745" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/45dc47fb-6d8a-44d4-8cbb-2e5791100144" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/7db1c869-e625-4558-9119-0f23da0f020c" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/71fc7616-d580-48fe-aa6d-1e6ceea41bdb" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/4098936e-a1b9-4971-901d-702ec390afa7" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/dd3dcce3-3d78-452f-9af0-27133497650c" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/2e356d04-5aaf-47d4-9bc3-4abba78cd18d" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/a9cbc57f-e9c1-4985-a6af-a98e5fce5ff3" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/e5c67781-0ffc-43c8-9c7d-333254eed704" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/93900e3c-79b5-4a05-bd6c-d68814e9e101" />

### Undertale (PlayStation 2) [Bytecode Version 16]

Here's a video :3 https://youtu.be/PuzBxe0VGtY

### DELTARUNE (SURVEY_PROGRAM) (PlayStation 2) [Bytecode Version 16]

Here's a video :3 https://youtu.be/TLJtV2WnrmQ

### DELTARUNE Chapter 2 (GLFW) [Bytecode Version 17]

<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/d0df9858-ad2b-4642-9f32-a542d1d942e0" />

### DELTARUNE Chapter 3 (GLFW) [Bytecode Version 17]

<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/7b49d434-e66f-4ee3-bfe8-c0b4f45ceeb7" />
<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/afbe62ad-4706-4882-a9c9-6c239ed57c69" />
<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/d83c9f8c-e9b9-410e-8d3d-3663ede23fab" />

### DELTARUNE Chapter Selector (GLFW) [Bytecode Version 17]

<img width="160" height="120" alt="image" src="https://github.com/user-attachments/assets/b8a848df-fd1c-49b7-9602-e8020ac86d5d" />
