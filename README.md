# GTA: San Andreas (Android 2.11.311) on Linux handhelds — XiFan RF35H

A Linux port of [gtasa_nx](https://github.com/NaGaa95/gtasa_nx) (the Nintendo
Switch loader for the Android build of GTA: San Andreas, itself derived from
fgsfds and Andy Nguyen's [gtasa_vita](https://github.com/TheOfficialFloW/gtasa_vita)): the original `libGame.so` (arm64-v8a, 2.11.311)
is loaded by a small ELF loader, linked against glibc, Mesa, OpenAL and
mpg123 through bionic-compatible shims, patched with gtasa_nx's hooks, and
driven through its `GameNative` JNI entry points. The target is the XiFan
RF35H (Rockchip RK3326: 4× Cortex-A35, Mali-G31 with Panfrost, 640×480,
~660 MB RAM) running Lakka with sway; nothing in the code is specific to it
beyond the defaults.

No game data is included or downloaded: the launcher installs the game from
the user's own APK and OBB. User documentation (Italian):
[`packaging/lakka/gtasa/LEGGIMI.md`](packaging/lakka/gtasa/LEGGIMI.md).

## How it fits together

```
RetroArch "Contentless Cores"
  └─ gtasa_libretro.so (launcher core) ── rf35h-gtasa import ──> gtasa --import, --check
        │ options → gtasa_nx.cfg
        └─ rf35h-gtasa start ──> rf35h-gtasa.service (Conflicts=retroarch.service)
                                     └─ rf35h-gtasa run ──> gtasa ──> RetroArch again
gtasa
  ├─ loader.c            ELF loader: RELA, RELR, Android packed (APS2) relocations
  ├─ tls_guard.c         bionic's stack-guard slot at TPIDR_EL0+0x28 on glibc
  ├─ bionic_*.c          bionic ABI on glibc: stdio/__sF, pthread (4-byte aligned
  │                      objects: 32-bit handles), semaphores, signals, setjmp,
  │                      sysconf/pathconf, fortify, ctype, locale, netdb
  ├─ import_table.c      what each import binds to; ABI-unsafe names never
  │                      fall through to glibc
  ├─ android.c           AAsset, ANativeWindow, log, properties
  ├─ casefold.c          names the game spells in another case than the disk
  │                      (gtasa_nx ran on FAT/exFAT; /storage is ext4)
  ├─ game_tuning.c       the game's first-run graphics settings: it would start
  │                      Panfrost at its top profile (see below)
  ├─ egl_wrap.c gl_wrap.c the game's EGL/GLES calls: platform window, swap
  │                      interval, redundant-state cache, counters
  ├─ platform.c          xdg-shell window sized from wl_output (sway configures
  │                      0x0 until the window maps), wp_viewporter scaling, SDL2 pads
  ├─ gamenative.c        engine lifecycle, input → GameNative entry points
  ├─ build_check.c       is this the 2.11.311 build the hooks expect?
  ├─ import.c zipx.c     APK/OBB installer (zip64, CRC-checked, one at a time:
  │                      flock on .import.lock, followed by the launcher)
  ├─ stats.c crash.c     per-interval measurements; crash reports with
  │                      module+offset+symbol backtraces (per-thread signal
  │                      stack, fault-free probing of the frame chain)
  └─ upstream/           gtasa_nx 3460c75, unmodified (tools/verify-upstream.sh)
```

RF35H defaults: 30 fps pacing on a 60 Hz panel, vsync, Mesa glthread, GL
state cache, performance governors for CPU/GPU/DDR (restored on every exit
path), gtasa_nx options tuned for a weak CPU (no off-screen persistence, no
ped specular), OpenAL Soft with a linear resampler at 44.1 kHz. All of them
are in `gtasa_nx.cfg`, written with comments on first run.

What the game binary itself showed (2.11.311, run under qemu with the
port's loader; `tests/test_game.c`, `tools/shaderdump.c`):

- First-run settings: `MobileSettings::SetRendererDefaults` only lowers them
  for Adreno 225 / PowerVR SGX 540 class GPUs (from the GL strings). With
  Panfrost's caps it starts at Visual FX High, real-time shadows, draw
  distance 100 and car reflections 3, which re-renders the scene into a
  1024x512 sphere map every frame. `game_tuning.c` keeps the game's logic
  and starts this device with the game's own weak-GPU profile (Visual FX
  low, no shadows, static reflections) and draw distance 50; the menu
  choices in `gta_sa.set` win from then on.
- The game's own resolution slider only applies above 640 pixels of width,
  so at 640x480 the 3D scene is always rendered at full size into an
  off-screen target and copied out; `render_scale` (compositor scaling) is
  the only resolution lever here.
- Shaders (571 variants in scache.txt, from the game's generator): fragment
  shaders are already `mediump`/`lowp`, so Panfrost already runs them in
  FP16; lighting is per vertex. 481 of them sample the diffuse texture with
  a -0.5 LOD bias, which `texture_lod_bias 0` removes (glShaderSource
  rewrite; all rewritten shaders validated as GLSL ES 1.00 with glslang).
- Texture databases: the dxt and etc variants are the same size (185 MB for
  gta3, 48 MB for gta_int), so which one Panfrost's extensions select makes
  no difference.

## Building

Cross-building on a PC (Ubuntu 24.04: `gcc-aarch64-linux-gnu`, the -dev
packages listed in `tools/cross-env.sh`, `wayland-scanner`,
`wayland-protocols`):

```sh
tools/cross-env.sh /tmp/xenv          # target library headers + link stubs
make CROSS=aarch64-linux-gnu- \
     PKG_CFLAGS="-I/tmp/xenv/include -I/tmp/xenv/include/SDL2" \
     PKG_LIBS="-L/tmp/xenv/lib -lSDL2 -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lopenal -lmpg123 -lz"
```

For Lakka, `tools/make-overlay.sh` produces `gtasa-rf35h-overlay.tar.gz`:
`install.sh /path/to/Lakka-LibreELEC` adds the package under
`projects/Rockchip/devices/RK3326/packages/gtasa` and enables it for
`UBOOT_SYSTEM=rf35h`; then build as usual (`scripts/build gtasa`, `make image`).

## Testing

```sh
BIONIC=~/src/bionic LAKKA=~/src/Lakka-LibreELEC BUSYBOX=/path/to/busybox tests/run-tests.sh
```

Needs qemu-user-static (aarch64), weston, and for the sway step sway and
grim (skipped without them). `GTASA_GAME_DIR=/path/to/your/files` adds the
checks against the game binary (glslangValidator validates the shaders).

| step | what it proves |
|---|---|
| vendored files | `upstream/` is byte-identical to gtasa_nx 3460c75 |
| ABI | `src/bionic_types.h` against bionic's own headers; 578 layouts and constants identical between bionic and glibc for the pass-through imports; bionic `FILE` is 152 bytes |
| fingerprints | the build-check instruction words are what GNU as encodes |
| cross build | aarch64, warnings are failures |
| loader (qemu, Cortex-A35) | a bionic-ABI library built by GNU ld and by clang/lld for Android (APS2 + RELR) loads, relocates and runs: relocations, errno, stdio, fortify entry points, ctype, sysconf, stat, readdir, names found whatever their case (fopen, open, openat, stat, lstat, opendir, AAsset with the data/ root spelled DATA/; a miss forgotten once its directory changes; a trailing slash still means a directory), mutexes, threads, timed waits (realtime and monotonic), mutex/cond/rwlock/semaphore at 4-byte alignment (qemu faults on misaligned atomics as the A35 does), bionic's 16 KiB stack minimum, setjmp, signals, the stack canary, crash reports (a garbage frame pointer, a stack overflow on a game thread), damaged ELF files refused, symbolization |
| refused import | the real `gtasa --import` under qemu while another import holds the lock: exit 3, the running import's log (not rotated) and status untouched |
| game binary (optional) | with `GTASA_GAME_DIR` set to your libGame.so, libc++_shared.so and scache.txt: build fingerprints, every import bound, the game's own first-run settings on Panfrost's caps, game_tuning's profile (-1, out of range, gta_sa.set present), all 571 shader variants with the bias removed, validated by glslang |
| imports | all 572 names gtasa_nx binds resolve (257 wrappers, 315 host), none unbound |
| platform (weston, sway) | fullscreen window, output mode read, EGL from a render thread, eglMakeCurrent filter, vsync on/off enforced, render scaling and setBuffersGeometry (0x0 keeps render_scale) checked on screenshots, gamepad mapping, dead zone, triggers, exit combination, pacing, sysfs settings restore, the pump returning on a dead connection; under headless sway (the compositor Lakka runs) on a rotated 600x800 output: starting size from wl_output, fullscreen once mapped, scaling on screenshots |
| GL cache | 20000 random steps in lockstep against direct GL: identical state after every step and identical images |
| import | APK + zip64 OBB + patch OBB install; 32-bit APK, missing OBB, path traversal, CRC damage and garbage refused; a second import exits 3 and touches nothing; the lock on the runner's descriptor outlives the importer |
| launcher | the libretro core under a fake frontend: install, launch, options, last error + retry, failures; an import from an earlier session followed (not restarted), a lost race (exit 3) followed, an interrupted import named, Restart during an import, START launching an installed game past a failed import, SIGCHLD ignored by the frontend (statuses unknown: the status file decides) |
| shader rewrite | `texture_lod_bias 0`: biased diffuse samples rewritten, anything else (other samplers, positive or non-literal bias) left alone, sources in pieces with lengths |
| runner | `rf35h-gtasa` under dash and busybox ash: options, runtime log (time from the uptime; directory from retroarch.cfg as RetroArch finds it), SIGTERM, import lock held through the check, import validation, slim |
| package | package.mk built the way LibreELEC builds it; the guard array opens the TLS block; the service gets every environment file RetroArch's unit has |

Each test that can pass vacuously has a mutation script (`tests/mutate_*.sh`)
that breaks the code it guards and requires the test to fail: 12 loader,
8 GL cache, 21 platform (2 of them under sway), 8 import, 15 launcher,
4 shader rewrite, 13 runner mutants, and 5 game mutants when the game
binary is available.

qemu does not model caches, so `so_flush_caches` covering only executable
segments (cache maintenance on the PROT_NONE gaps between segments faults
on the A35) is checked by reading, not by a test.

What only the device can show is listed in LEGGIMI.md and in the stats log:
memory headroom from the menu into the city, the GL extensions and the
texture database variant Panfrost leads the game to (`gtasa.log`), frame
times, and whether the game is CPU- or GPU-bound (`stats.log`).

## Licenses

MIT (this port and gtasa_nx, see `upstream/LICENSE`); `libretro.h` is MIT
(RetroArch team). The game is Rockstar Games' and is not distributed.
