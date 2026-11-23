# Twisted Metal: Head-On – Extra Twisted Edition — PC Port

A native Windows port of **Twisted Metal: Head-On – Extra Twisted Edition** (PlayStation 2,
SCUS-97621, NTSC-U).

This is not an emulator running the game. The game's own PS2 program (`SCUS_976.21`, about 7,260
functions) is **statically recompiled to C++** with [PS2Recomp](https://github.com/ran-j/PS2Recomp)
and built into a normal PC program, `TMHO.exe`. Around it sits a runtime that does what the
PS2's other chips did: graphics, sound, the I/O processor, the vector units, the disc and the
memory cards.

> **No game data is included.** You need your own copy of the game. The port reads the game's
> files from your own disc image, and the recompiled code is made on your own machine from your
> own copy. Nothing from the disc is in this repository.

---

## Getting started (players)

1. Download a release and unpack it into a folder of its own (or run `Install.bat` if the
   release comes with one).
2. Start **`TMHO.exe`**. The setup screen opens.
3. Pick your disc image (`.iso` of SCUS-97621) with **Browse…**, or drag it onto the window. An
   image next to `TMHO.exe`, or in a `Game Rom` folder one or two levels up, is found by itself.
4. Click **Build game**. It copies the game's files from the disc into a `game` folder (about
   4.1 GB) and checks that the game program is the version the port was made from.
5. Optional: click **Build shaders**. It prepares all of the GPU renderer's shader variants
   (10,904 of them) and saves them as `shadercache.bin` next to `TMHO.exe`, so the game starts
   without compiling them and no effect stutters the first time it appears.
6. Click **PLAY**.

Needs Windows 10/11 (64-bit) and a graphics card with Vulkan. Keep the `.iso` where it is: the
game still streams music and movies from it. Saves go in the `saves` folder next to `TMHO.exe`.

### Controls

| PS2 | Keyboard |
|---|---|
| D-pad | Arrow keys or W A S D |
| Cross / Circle / Square / Triangle | X or Space / C or Esc / Z / V |
| L1 / R1 / L2 / R2 | Q / E / Left Shift / Right Shift |
| Start / Select | Enter / Tab |
| Settings | **F2** |
| Debugger | F1 |

A controller works too; the keyboard keeps working alongside it.

---

## What the PC port includes

### The game itself
- The complete game program, recompiled and running natively: menus, movies, cutscenes and all
  game modes are part of it. Most of the testing so far has been Challenge matches on Big Blue
  Stadium and the movies; reports from other modes and battlegrounds are welcome.
- **Setup screen** that builds the game from your disc image, checks the version, finds your
  disc by itself, reuses the files of another copy of the port on the same PC, builds the
  shader cache, and can be skipped at start.
- **Memory cards** saved as files (`saves\mc0`, `saves\mc1`).

### Graphics
- **Vulkan GPU renderer** ([paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs)), with
  a **software (CPU) renderer** as a fallback.
- **Internal resolution** 1x (native 640×448), 2x or 4x, or **Auto** (2x up to 1080p, 4x above).
  Changes take effect immediately.
- **True 16:9 widescreen**: the 3D cameras see more to each side instead of stretching the
  picture; the HUD, radar, weapon icons, names and text are moved and resized to fit; movies stay
  4:3. Also **4:3** (original) and **Stretch**.
- **Display modes**: Windowed, Borderless and exclusive **Fullscreen**. Resolutions from
  640×480 (4:3 native) and 854×480 (16:9) up to 4K (3840×2160), plus your monitor's own display
  modes in Fullscreen. Everything switches **live, without restarting the game**.
- Scaling filter (smooth or sharp) and V-Sync.
- **Lighting fix**: the COP2 instruction fix from PCSX2's game database, which corrects
  lighting that came out wrong when the vector unit was read too early.

### Camera
- **Field of view** slider (50–120°; the game's own view is 65° at 4:3 and 81° at 16:9).
- **Camera distance** for the chase camera (50–200%).
- Both work live in a match, and the HUD's 3D radar and weapon icons keep their place and size
  at any field of view.

### Sound
- The PS2 sound chip (**SPU2**) emulated, driven by the game's own sound driver (989snd) from
  the disc: music, menu sounds, engine sounds, guns, weapons, explosions and the announcer.
- **Movies and cutscenes** decoded with FFmpeg, with their audio in sync.
- Master volume and **mute when the window is in the background**.

### Performance
- **Full speed in big battles.** On a Ryzen 7 3700X with an RTX 2080 SUPER (16:9, Auto/2x
  internal resolution), Challenge matches on Big Blue Stadium with **three opponents now run at
  100% speed / 60 fps** (they ran at 70–85% before), and with **five opponents** at 100% nearly
  all the time, with an occasional two-second stretch at 92–99% in the heaviest explosions.
- **Both vector units' microprograms are compiled ahead of time** to native code, and checked
  bit for bit (registers, memory, flags, graphics output and cycle counts) against runs recorded
  from the game:
  - **VU1** (3D geometry) uses a two-tier compiler: a static tier that works out the VU1's
    pipeline stalls, flag timing and branch delay slots while compiling, so the common paths run
    with no runtime pipeline bookkeeping, and a dynamic tier for everything else. In recorded
    runs it takes 3.1 ns per VU cycle: **2x faster** than the previous compiled code (6.5 ns)
    and 6.5x faster than the interpreter core (20 ns).
  - **VU0** (the game's physics and maths helpers, called about 80,000 times a second in a big
    battle) is compiled the same way, keeping every flag the CPU reads back: **2.2x faster** in
    recorded runs, and its share of the game thread fell from about 150–210 to 75–100 ms per
    second in a five-opponent battle.
- The VU1 runs on a **CPU core of its own**, and a **GS feed thread** parses the graphics
  packets and hands them to the GPU on another core.
- **Shader cache**: the GPU renderer's pipelines are kept in `shadercache.bin` (made by Build
  shaders on the setup screen, and topped up by the game when it compiled anything new), so the
  start-up precompile is a cache load: all 10,904 variants load in under a second.
- The performance log's profiler samples the VU1 thread as well as the game thread, and its
  timers use QueryPerformanceCounter directly (cheaper than the C runtime clock it used before).

### Accuracy
- The PS2 CPU's floating-point rules are reproduced: no infinities or NaNs, clamping to the
  largest number instead, square roots of the absolute value, division by zero giving ±max.
- **Round-toward-zero** rounding and truncating float-to-integer conversion, like the real
  hardware (the PC rounds to nearest by default). This affects physics and collision.
- The vector units use the PS2's own rounding and clamping as well.

### Settings menu (F2)
- **Video**: display mode, resolution, aspect ratio, internal resolution, scaling, V-Sync,
  renderer.
- **Audio**: master volume, mute in the background.
- **Game**: pause while the menu is open, FPS counter, screenshot recording, VU1 thread, game
  disc, field of view, camera distance, quit.
- The **setup screen** (F2 > Game > Game disc > Setup, or "Show this screen at start") has the
  disc, Build game and Build shaders.
- **UI**: accent colour, menu opacity, which side the menu opens on.
- Settings are kept in `settings.ini`.

### Tools for testing and bug reports
- **FPS counter**: the game's real frame rate, the window's frame rate and the game speed.
- **Logs** folder: `performancelogs.txt` (speed every 2 seconds, time per thread, what holds the
  game back, a built-in profiler), `Graphicslogs.txt` (GPU work, frames, video memory) and
  `console.txt` (the game's own text output).
- **Freeze watchdog**: if the game hangs it writes where it got stuck, and if closing takes
  more than 10 seconds it ends itself.
- **Screenshot recorder** (F2 > Game > Record screenshots): a picture every half second in
  `Logs\frames`, for bug reports.
- **F1 debugger**: threads, I/O processor, RPC history, GS registers and memory.

---

## Known issues

- **The heaviest five-opponent moments can dip briefly** (92–99% speed for a couple of
  seconds on a Ryzen 7 3700X). The game thread and the VU1 thread are both close to a full core
  then, and the game waits for the VU1 to finish each frame's drawing before it starts the next.
  Slower CPUs will see more of this.
- **No 120 fps yet.** The game's logic, physics and animations are tied to the PS2's 60 Hz
  frame clock, so a higher frame rate needs changes inside the game's systems, not just a
  setting (see the plans below).
- **Menu artwork is stretched in 16:9.** The 3D views and the HUD are true widescreen, but the
  full-screen 2D menu backgrounds are drawn for 4:3.
- **Falling through the arena** (player and AI cars) was reported in earlier versions. Version 6
  switched the game's floating-point maths to the PS2's own rounding, the most likely cause; if
  it still happens, please report where.
- **Reverb** and some sound-envelope details of the SPU2 are not emulated yet, so a few sounds
  are drier than on a PS2.

## Coming in the future

- **More headroom in the heaviest battles**: keeping VU registers in CPU registers across
  compiled blocks, faster VIF1 unpacking, a faster I/O-processor core, and overlapping more of
  each frame's VU1 work with the game's own work, so slower CPUs stay at 60 fps too.
- **Native 120 fps** (and a frame-rate setting): running the game's frame timing, physics steps
  and animations at 120 Hz without speeding the game up and without interpolated frames.
- **Widescreen menus**: the 2D menu screens kept at their shape (pillarboxed) or adapted to 16:9.
- **More accurate sound**: SPU2 reverb, volume envelopes and the PS2's own sample
  interpolation.
- **Collision follow-up**: tracking down any remaining fall-through spots.
- **Controls menu**: remapping keys and controller buttons in F2.
- **Linux release** (the code already builds and runs on Linux).

---

## Building from source

Everything needed to build the port is here except the game itself. See
[`port/BUILDING.md`](port/BUILDING.md); in short, on a Linux host:

```sh
git clone --recursive https://github.com/sn0wdnmail-lab/TwistedMetalHead-On-Port.git
cd TwistedMetalHead-On-Port
FFMPEG_ROOT=/path/to/ffmpeg-mingw port/build.sh /path/to/your/SCUS_976.21 windows
```

### Repository layout

| Path | What it is |
|---|---|
| `ps2recomp/` | PS2Recomp (recompiler, runtime, I/O processor emulator) with all of the port's changes |
| `ps2recomp/ps2xRuntime/src/lib/tmho_game.cpp` | Head-On specific code: widescreen, HUD placement, field of view, camera distance |
| `ps2recomp/ps2xRuntime/third_party/parallel-gs` | paraLLEl-GS (git submodule), patched by `port/patches/parallel-gs.patch` |
| `port/tmho.toml` | Recompiler configuration for SCUS_976.21: function map, SDK bindings, instruction patches |
| `port/tmho_functions_named.csv` | Function boundaries and names used by the recompiler |
| `port/build.sh` | Recompiles your executable and builds `TMHO.exe` (or a Linux binary) |
| `port/package/` | Packaging script and the player README that goes with a release |
| `port/testing/` | Scripts that drive the game unattended for tests |
| `port/tools/` | Analysis helpers (IRX and VU disassemblers, disc map, config generator) |

## Credits and licences

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors — the recompiler and
  runtime this port is built on. GPL-3.0; the port's changes to it are under the same licence
  (see `LICENSE`).
- [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) by Arntzen Software (with
  Granite) — the Vulkan GS renderer. LGPL-3.0.
- [raylib](https://github.com/raysan5/raylib) and GLFW (window, input, audio output),
  [Dear ImGui](https://github.com/ocornut/imgui) with [rlImGui](https://github.com/raylib-extras/rlImGui)
  (debugger), [FFmpeg](https://ffmpeg.org) (movie decoding, LGPL), plus ELFIO, toml11, fmt,
  rabbitizer and libdwarf in the recompiler.
- [PCSX2](https://pcsx2.net): the COP2 lighting fix for this game comes from its game database
  (by refraction), and its widescreen patch for the game was the reference for the HUD
  positions.

Twisted Metal is a trademark of Sony Interactive Entertainment. This project is not affiliated
with or endorsed by Sony. It contains no game code, data or assets; use it only with a copy of
the game you own.
