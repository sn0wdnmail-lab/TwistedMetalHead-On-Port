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
| Windowed ↔ full screen | F11 or Alt+Enter |
| Screenshot | F12 |
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
- **Resolution presets** (V7): **480p** (1x, the PS2's own), **720p**, **1080p**, **1440p** and
  **2160p (4K)**, whatever the screen is. A preset above the screen's own is drawn big and shrunk
  to fit (smoother edges). Changes take effect immediately.
- **Frame rate** (V7): **60 fps** (the game's own frames), **120 fps**, or **Monitor** (the
  screen's refresh rate, e.g. 144 or 170 Hz). 120 and Monitor add in-between frames made on the
  GPU from the two newest game frames (motion-compensated), so motion is smoother while the game
  keeps its normal speed; the picture is about one frame later. With V-Sync on, Monitor mode
  waits for the desktop compositor in windowed and borderless mode, because some drivers ignore
  the swap interval there.
- **Steady picture** (V7.2): the game draws each frame for one of the two interlaced video
  fields, which sit one line apart. paraLLEl-GS showed a frame on whichever field the video
  output had reached by then, so a frame that came late or was shown twice landed on the wrong
  field, and the whole picture jumped a line up or down now and then. The renderer now notes
  which field each frame buffer was drawn for (from the game's drawing offset) and shows it on
  that field; `Graphicslogs.txt` counts how often that happens. In five-opponent battles on the
  test PC (1440p, 120 fps) it caught about 430 such frames in under six minutes of play, up to
  35 in two seconds in busy moments: each of them used to be a one-line jump.
- **True 16:9 widescreen**: the 3D cameras see more to each side instead of stretching the
  picture; the battle HUD sits right at the screen edges, and the radar, weapon panel, names and
  text keep their shape; movies stay 4:3. Also **4:3** (original) and **Stretch**.
- **Display modes**: Windowed, Borderless and exclusive **Fullscreen**; **F11** or **Alt+Enter**
  switches between windowed and full screen at any time. Everything switches **live, without
  restarting the game**.
- Scaling filter (smooth or sharp) and V-Sync.
- **Replaceable font** (V6.2, Vulkan renderer): a `textures/font_atlas.png` next to TMHO.exe
  (the game's 16×16 grid of letters, upright, redrawn at 2, 4, 8 or 16 times the size) is used
  for every piece of text instead of the game's 512×512 font texture. It is mipmapped, so it
  stays clean at every internal resolution. Since V6.3 its text is shaded per sample at 2x and
  4x (paraLLEl-GS otherwise shades sprites once per native pixel), so letters get sharper with
  the internal resolution instead of losing thin strokes. No font picture ships with the port.
- **Lighting fix**: the COP2 instruction fix from PCSX2's game database, which corrects
  lighting that came out wrong when the vector unit was read too early.

### Battle HUD layout (F2 > HUD)
- **HUD editor**: the radar and the weapon panel are shown over your own game picture; drag a
  piece to move it anywhere, scroll or drag a corner to resize it (40–250%), arrow keys nudge it.
  Changes show live in the game behind the editor.
- **Snap to screen edges** moves both pieces into the corners, keeping their sizes; **Reset**
  puts them back (screen edges in 16:9, the game's own place in 4:3).
- A moved or resized weapon panel keeps the look the game gave it: the panel is drawn as the
  game placed it and then slid and scaled on screen as a whole, so the special-weapon icon stays
  centred in its frame wherever the panel goes (its parts lie at different depths, so simply
  moving the 3D model showed it from another angle and pushed the icon off-centre).
- The ammo and cars-left numbers keep their 4:3 width in 16:9, so a thin "1" no longer drops
  out between the font texture's texels.
- **HUD text no longer glitches** (V7.2): a text call (the cars-left counter in the top-left
  corner, for one) sometimes stops part-way through its string for a VBlank or timer interrupt
  and carries on afterwards. The port set the game's own font size back as soon as the call
  first stopped, so the rest of the string came out at the wrong size and place for a frame.
  The size now stays until the call really finishes.

### Camera
- **Field of view** slider (50–120°; the game's own view is 65° at 4:3 and 81° at 16:9).
- **Camera distance** for the chase camera (50–200%).
- Both work live in a match, and the HUD's 3D radar and weapon icons keep their place and size
  at any field of view.

### Sound
- The PS2 sound chip (**SPU2**) emulated, driven by the game's own sound driver (989snd) from
  the disc: music, menu sounds, engine sounds, guns, weapons, explosions and the announcer.
- **Clean sound (V7)**: the SPU2's own Gaussian sample interpolation, volume slides done as the
  chip does them, and a soft limiter instead of hard clipping when loud explosions add up. The
  I/O processor's clock now follows the game's exactly (it used to run 2–7% fast in battles, so
  sound piled up), and the buffer to the sound card follows the card's clock with small, smooth
  speed corrections (time-stretching only if the clocks drift far apart). In a battle the sound
  runs within ±0.3% of real time with no dry-outs.
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

#### Version 7.1: the I/O processor on its own core
The PS2's I/O processor (it runs the 989snd sound driver and the SPU2 mixing, the disc and the
memory cards) used to run on the game thread. It now runs on a thread of its own, and mostly
while the game thread is waiting for its next VBlank: when the game goes idle, the I/O processor
is brought up to the game's time and then runs ahead into the idle time, up to the next VBlank
or timer. Whenever the game talks to it (an RPC, a SIF transfer, I/O processor memory), it is
caught up to the game's exact time first, and what the I/O processor thread writes to the EE's
side is applied on the game thread between two pieces of game code, so the game sees the same
order of events as before. F2 > Game > **IOP thread** (on by default) turns it off.

Measured on the same PC as above, the same five-opponent Challenge on Big Blue Stadium (same
opponents), 2160p, frame rate Monitor (170 Hz), the same build with the IOP thread off and on,
about 2.5 minutes of battle each:

| | IOP thread off | IOP thread on |
|---|---|---|
| Game speed / window | 100% / 170 fps | 100% / 170 fps |
| I/O processor work on the game thread | 80 ms per second (up to 126) | 2 ms per second (up to 7) |
| Game thread busy | 527 ms per second on average, 768 at the peak | 460 ms per second on average, 643 at the peak |
| IOP thread | – | 66 ms per second, 61 of them while the game waited for its next frame |
| Sound | tempo within ±0.35%, no dry-outs | tempo within ±0.5%, no dry-outs |

- The settings menu packs a tab's rows a little closer when it has many of them, so the help
  text under the last row stays on screen.

#### Version 7: full speed at 4K, and lighter on the CPU
Measured on a Ryzen 7 3700X with an RTX 2080 SUPER, Challenge on Big Blue Stadium against five
opponents, 16:9, **2160p** resolution, frame rate **Monitor (170 Hz)**:

| | V6.3 / early V7 | V7 |
|---|---|---|
| Game speed | lagging at 4K | 99–100%, window at 170 fps |
| Slowest frame in each 2-second report | 23–29 ms (a hitch every report) | 6.4–6.6 ms (one 170 Hz refresh) |
| Game thread waiting for the GPU | up to 330 ms per second | 0 |
| VU1 microcode (VU1 thread) | 420–690 ms per second | 250–420 ms per second |
| VU0 microcode (game thread) | 80–110 ms per second | 50–70 ms per second |

- **4K lag fixed**: once a frame the game reads 8 depth values back from the GS (to dim the
  sun's lens flare when something covers it). That read made the game wait until the whole frame
  so far was drawn on the GPU. The answer now comes from the previous frame while the real read
  runs in order behind the drawing (for DMA and for FIFO reads), and the GPU gets its work in
  smaller batches during super-sampled frames, so it never sits idle.
- **No more hitch every two seconds**: the performance report (a census of all threads, the
  profiler and the file writes) ran on the window thread and held one frame up by about 17 ms
  each time; it has its own thread now.
- **Compiled VU code 1.6x faster** (3.05 → 1.87 ns per VU cycle in recorded runs, all of them
  still bit-exact): no result clamp where round-to-zero already keeps results finite, operand
  normalisation and MAC/status flags in a few SIMD instructions, one normalisation shared by
  every lane taken from a register, masked writes as single BLENDPS instructions, vectorised
  FTOI and CLIP, and flags no longer committed before every flag-writing instruction.
- **Hybrid CPUs**: the game asks Windows not to treat it as background work (no EcoQoS, so it
  stays on the fast cores of Intel 12th-gen and later laptops), and its game, VU1, GS feed and
  window threads run slightly above normal priority.
- Tidier logs: the game's GS interrupt-mask calls are no longer logged several times a second.

#### Version 6.1: less CPU for the same 60 fps
Measured on the same PC (Ryzen 7 3700X, 16 hardware threads; RTX 2080 SUPER; 16:9, Auto
resolution), from the port's own performance log (CPU time of every thread of the game):

| | V6 | V6.1 |
|---|---|---|
| Whole game in a Challenge battle | 1.5–1.8 cores | 1.1–1.3 cores with one opponent, 1.5–1.65 in the heaviest five-opponent fights |
| GS feed thread | 0.40–0.44 cores (woken 47,000–88,000 times a second) | 0.19–0.30 cores (about 3,000 wake-ups a second) |
| GPU queue submissions | about 1,100 a second | about 480 a second |

- **GS feed thread wakes in batches**: the VU1 thread hands graphics packets over in groups
  (24 packets / 96 KB, every vsync, and whenever the game reads a GS register) instead of waking
  the feed thread for each of its ~120,000 packets a second.
- **Native Windows locks**: the threads that hand work to each other use Windows' own slim locks
  and condition variables instead of the MinGW pthread emulation.
- **One GPU submission per frame**: paraLLEl-GS used to hand queued work to the GPU every 4,096
  primitives (three or four times a frame); a battle frame now goes in one piece.
- **Frame hand-off without a CPU pass**: the finished frame is copied as it is and the colour
  channels are swapped by a tiny shader on the GPU, instead of a per-pixel loop on the CPU.
- **Faster compiled VU1 code** (about 10%): broadcast operands are loaded as whole registers and
  shuffled, avoiding a store-to-load forwarding stall on every such instruction.
- **I/O processor fast paths**: cached import-stub lookups and a cheaper DMA check.
- Per-frame GPU images without dedicated driver allocations, and **instant exit** (no more
  `TMHO.exe` left hanging in a graphics driver on quit).
- **CPU-limit test**: with the whole game capped at 10% of the CPU (1.6 of the 16 hardware
  threads, `cpu_limit_percent=10` in `settings.ini`, a Windows job-object hard cap), battles
  with one opponent and normal play stay at 60 fps with no frame over 25 ms; the heaviest
  five-opponent fight dipped to 43–56 fps for about 14 seconds, when the game needs ~1.6 cores.
  The cap is off by default.

### Accuracy
- The PS2 CPU's floating-point rules are reproduced: no infinities or NaNs, clamping to the
  largest number instead, square roots of the absolute value, division by zero giving ±max.
- **Round-toward-zero** rounding and truncating float-to-integer conversion, like the real
  hardware (the PC rounds to nearest by default). This affects physics and collision.
- The vector units use the PS2's own rounding and clamping as well.

### Settings menu (F2)
- **Video**: display mode, resolution, aspect ratio, scaling, V-Sync, renderer, frame rate.
- **Audio**: master volume, mute in the background.
- **Game**: pause while the menu is open, pause in the background, FPS counter, screenshot
  recording, screenshot, VU1 thread, IOP thread, game disc, field of view, camera distance,
  quit.
- **HUD**: the HUD editor, radar and weapon panel sizes, snap to screen edges, reset.
- The **setup screen** (F2 > Game > Game disc > Setup, or "Show this screen at start") has the
  disc, Build game and Build shaders.
- **UI**: accent colour, menu opacity, which side the menu opens on.
- Settings are kept in `settings.ini`.

### Tools for testing and bug reports
- **FPS counter**: `FPS - 60` in the corner: pictures on screen per second (in-between frames
  included when they are on).
- **Screenshots** (V7): **F12** saves the game picture, without menus or the FPS counter, as a
  PNG in `Screenshots` next to `TMHO.exe`, named by date and time. (raylib's own F12 and
  Ctrl+F12 GIF recorder are switched off.)
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

- **The heaviest five-opponent moments need about 1.6 cores, spread over four threads** (game
  thread up to ~0.65, VU1 thread ~0.55, GS feed ~0.35, IOP thread ~0.1 on a Ryzen 7 3700X). The
  game thread is the busiest single one: a CPU about half that fast per core is at its limit
  there; lower resolutions help the GPU but not these threads.
- **The software (CPU) renderer's text looks rough**: it samples textures without filtering, so
  thin strokes of the small HUD font can drop out. The Vulkan renderer filters them.
- **120 fps and Monitor use in-between frames.** The game's logic, physics and animations still
  run at the PS2's 60 Hz; the extra frames are made from the two newest game frames, so fast
  edges can show slight warping and the picture is about one frame later.
- **Menu artwork is stretched in 16:9.** The 3D views and the HUD are true widescreen, but the
  full-screen 2D menu backgrounds are drawn for 4:3.
- **Falling through the arena** (player and AI cars) was reported in earlier versions. Version 6
  switched the game's floating-point maths to the PS2's own rounding, the most likely cause; if
  it still happens, please report where.
- **Reverb** of the SPU2 is not emulated yet, so a few sounds are drier than on a PS2.
- **Music, effects and movie volume** sliders are shown but not working yet (they need a
  per-voice mixer).

## Coming in the future

- **More headroom in the heaviest battles**: faster VIF1 unpacking, cheaper MAC/status flags in
  compiled VU code, and overlapping more of each frame's VU1 work with the game's own work, so
  slower CPUs stay at 60 fps too.
- **Native 120 fps**: running the game's frame timing, physics steps and animations at 120 Hz
  without speeding the game up and without in-between frames.
- **Widescreen menus**: the 2D menu screens kept at their shape (pillarboxed) or adapted to 16:9.
- **A true 16:9 frame** (854 pixels wide instead of 640), so text and HUD no longer need squeezing
  to keep their shape.
- **More accurate sound**: SPU2 reverb, and separate music, effects and movie volumes.
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
| `ps2recomp/ps2xRuntime/src/lib/tmho_game.cpp` | Head-On specific code: widescreen, HUD placement and layout, field of view, camera distance |
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
