# Building the Head-On PC port

The build turns **your own copy** of the game's executable (`SCUS_976.21` from the SCUS-97621
disc) into C++ with the recompiler, then compiles that together with the runtime into
`TMHO.exe` (Windows, cross-compiled on Linux) or a native Linux binary. The generated C++ is the
game's code, so it is made on your machine and never committed here.

Tested on Ubuntu 24.04 (x86-64).

## 1. Packages

```sh
sudo apt install git cmake ninja-build python3 clang zip \
    libvulkan-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev \
    libavcodec-dev libavformat-dev libswscale-dev libswresample-dev          # Linux build
sudo apt install g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix      # Windows build
```

## 2. The sources

```sh
git clone --recursive https://github.com/sn0wdnmail-lab/TwistedMetalHead-On-Port.git
cd TwistedMetalHead-On-Port
```

`ps2recomp/ps2xRuntime/third_party/parallel-gs` is a git submodule (paraLLEl-GS with its Granite
submodule). `port/build.sh` checks it out and applies `port/patches/parallel-gs.patch`, which makes
paraLLEl-GS start a new frame context only at real frame boundaries (a large speed-up in this
integration). Other libraries (raylib, Dear ImGui, rlImGui, toml11, ...) are downloaded by CMake.

## 3. FFmpeg for Windows (Windows build only)

The movies are MPEG-2; the Windows build links a small static FFmpeg. With FFmpeg 7.1 sources:

```sh
./configure --prefix=$HOME/ffmpeg-mingw --arch=x86_64 --target-os=mingw32 \
    --cross-prefix=x86_64-w64-mingw32- --cc=x86_64-w64-mingw32-gcc-posix \
    --cxx=x86_64-w64-mingw32-g++-posix --pkg-config=pkg-config \
    --enable-static --disable-shared --disable-programs --disable-doc --disable-debug \
    --disable-everything --enable-decoder='mpeg2video,mpeg1video' --enable-parser=mpegvideo \
    --enable-demuxer='mpegps,mpegvideo' --enable-protocol=file --enable-avcodec --enable-avformat \
    --enable-swscale --enable-swresample --disable-network --disable-autodetect --enable-w32threads
make -j"$(nproc)" && make install
```

## 4. Build

```sh
FFMPEG_ROOT=$HOME/ffmpeg-mingw port/build.sh /path/to/SCUS_976.21 windows
# or
port/build.sh /path/to/SCUS_976.21 linux
```

The script builds the recompiler, recompiles the executable with `port/tmho.toml` into
`ps2recomp/ps2xRuntime/src/runner`, builds the program, and for Windows packages it into
`build/package/TMHO.zip` + `Install.bat` (`port/package/package.sh`). Compiling the roughly
7,260 generated files takes a while; the runtime's unity build (32 files per compile unit) keeps
it manageable.

## 5. Optional: VU1 programs compiled ahead of time

`ps2recomp/ps2xRuntime/src/lib/vu/vu1_aot_programs.cpp` is an empty table here, because the real
one is made from the game's VU1 microcode. Without it every VU1 program runs on the fast
interpreter core (same results, slower in big battles). To make it:

1. Run the game with `PS2X_VU1_CAPTURE=1` and play the parts you care about (menus, a few
   battles). The code images it sees are saved under `Logs/vu1capture`.
2. Build the generator and run it:
   ```sh
   cmake --build build/linux --target vu1aot_gen
   build/linux/ps2xRuntime/vu1aot_gen -o ps2recomp/ps2xRuntime/src/lib/vu/vu1_aot_programs.cpp Logs/vu1capture
   ```
3. Build again. `vu1_replay` (same folder) can check compiled programs against the recordings.

## Useful environment variables

| Variable | Effect |
|---|---|
| `PS2X_RENDERER=software` | software GS renderer instead of Vulkan |
| `PS2X_GS_THREAD=0` | parse GS packets on the VU1 thread instead of a thread of their own |
| `PS2X_MTVU=0` | run VIF1/VU1 on the game thread |
| `PS2X_EE_ROUND=nearest` | the PC's round-to-nearest instead of the PS2's round-toward-zero |
| `PS2X_VU1_AOT=0` | no ahead-of-time VU1 programs |
| `PS2X_RECORD_FRAMES=1` | screenshot every 0.5 s into `Logs/frames` |
| `PS2X_TMHO_TRACE=1` | log the widescreen / field-of-view / camera hooks |
| `PS2X_LOG_DIR=<dir>` | where the logs go |
| `PS2X_EXIT_AFTER=<s>` | quit after that many seconds (unattended tests) |

`port/testing/` has scripts that drive the Linux build under Xvfb for unattended tests.
