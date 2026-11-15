# curtimefix

Client mod for Counter-Strike: Source (64-bit, Linux and Windows) that keeps movement smooth no matter how long
the map has been running. Without it, the camera, your gun and every other moving thing start to stutter after
about an hour on the same map, and it gets worse every hour. Reconnecting or restarting the game doesn't help;
until now only a map change did. With curtimefix the game draws motion as smoothly as on a freshly loaded map.

It only changes what your client draws. Movement, prediction, hit registration and the server are untouched, the
server doesn't need anything installed, and other players don't need it.

Downloads are on the [releases page](https://github.com/brybry333/curtimefix/releases).

**Only use it with `-insecure`.** It hooks the game process, which is a VAC risk on secured servers.

Licensed under GPL-3.0, see `LICENSE`.

## Install

**Linux:** put `libcurtimefix.so` in a folder without spaces in its path and set CS:S's launch options to

```
LD_PRELOAD="/full/path/to/libcurtimefix.so:$LD_PRELOAD" %command% -insecure
```

**Windows:** keep `curtimefix.exe` and `curtimefix.dll` in the same folder and run `curtimefix.exe`. It starts
CS:S with `-insecure` (your Steam launch options still apply) and loads the DLL. If CS:S is already running with
`-insecure` (for example started by CSTV.exe), it loads the DLL into that game instead. It refuses a game that
was started without `-insecure`.

Check it in the game console: `curtimefix` prints the map time, the float step and what was fixed.

## The problem

The game keeps map time (`gpGlobals->curtime`) as a 32-bit float: seconds since the map loaded. A float has 24
bits of precision, so the bigger the number, the bigger the smallest step it can take:

| Map running for | Smallest time step |
|---|---|
| 34 min (2048 s) | 0.24 ms |
| 68 min (4096 s) | 0.49 ms |
| 2.3 h (8192 s) | 0.98 ms |
| 4.6 h (16384 s) | 1.95 ms |
| 9.1 h (32768 s) | 3.9 ms |
| 18.2 h (65536 s) | 7.8 ms |

Every rendered frame, the client blends everything that moves between the samples it got from the server
(`CInterpolatedVar`), and it does that math with times of this size. At 1000 fps a frame lasts 1 ms, so once the
step gets near that, the time used for the blend comes out in uneven chunks: some frames move too far, some don't
move at all. At 4.6 h about half the frames at 1000 fps don't move. Mouse look stays smooth (view angles aren't
blended), but anything you see moving, including your own position, stutters.

The client's clock is the server's map time, so reconnecting or restarting the game doesn't reset it. Movement
physics aren't affected: player commands and prediction use tick-based time that rounds the same way on both sides.

## What curtimefix changes

### 1. Interpolation of everything that moves

The camera, players, doors, platforms and ragdolls all go through the seven `CInterpolatedVarArrayBase<T>::Interpolate`
implementations. curtimefix finds them at startup (RTTI names → vtables → the `Interpolate` thunk → the
implementations) and redirects every call/jmp into them through a small stub.

Around each call:
1. It checks that the time the game passed is this frame's render time: exactly
   `(tick + interpolation amount) × tick interval`. Anything else is left alone.
2. It shifts that time and every stored sample time down by the same base, so they're all about 1 s. Each sample's
   exact time is recovered on the way (server samples are tick multiples).
3. It runs the game's own `Interpolate` on those small, exact numbers, then puts every stored time back bit-exact.

So the game's own blending code does the work, only on numbers where the float is precise.

### 2. Gun sway

The viewmodel's sway history stores the float time of the frame each sample was taken in. At 1000 fps two frames
can share one float value, so the sway jumps. curtimefix keeps the exact time of each frame's sample (a small record
per object) and uses that.

### 3. Snaps after boosters, teleports and landings

When a new sample arrives, the game drops an older one with the "same" time using `t + 0.0001f > new`. Past 34 min
the float step is bigger than 0.0001 s, so `t + 0.0001f` rounds back to `t`, the test fails, and a stale sample
stays in. The camera then snaps when your velocity changes suddenly. curtimefix hooks `NoteChanged` and does the
test in exact time before the game's own runs.

### 4. Gun animation speed

`C_BaseViewModel::Interpolate` sets the animation cycle from `elapsed = curtime − m_flAnimTime + amount`: two
big floats subtracted, off by up to a float step every tick. While that function runs (vtable slot 143 on Linux,
97 on Windows), curtimefix shifts the local player's final predicted tick and `m_flAnimTime` down to small exact
values (the animation lands 128 ticks in, like on a fresh map) and puts them back afterwards. It first checks for
30 frames that the field really holds the predicted tick, and leaves this part off if it doesn't.

### Finding things at runtime

Everything is found from class names and code shapes, not fixed addresses: the `Interpolate` implementations and
their call sites, the `NoteChanged` slot, the gun's vtable, and the client clock (through `IEngineTool`'s
ClientTime/FrameTime/Tick accessors). Netvar offsets come from the RecvTables. If something no longer matches after
a game update, it says so in the console and leaves that part alone. `tools/check_build.py` runs the same discovery
on the game files.

## Measured (build 11003710)

Camera speed variation from frame to frame while flying at constant speed (standard deviation / mean), with
curtimefix on / off, in the same session:

| Map time | Linux, listen server, ~900 fps | Windows (Proton), ~2600 fps |
|---|---|---|
| just loaded | 0.02% / 0.12% | 0.07% / 1.07% |
| 1.5 h | 0.01% / 18% | 0.15–0.29% / 40–45% (9–18% of frames frozen) |
| 2.5 h | 0.01% / 29% (6% frozen) | |
| 5.6 h | 0.01% / 103% (50% frozen) | |

- 100-tick dedicated server, ~1000 fps, stock client: 0.8% fresh, 10.7% at 45 min, 20% at 90 min, 34.8% at 150 min,
  106% at 5.5 h. With curtimefix two reruns stayed at the fresh-map level at every age. At 37 min, the camera's
  median deviation within a tick was 0.5% with curtimefix and 9.7% without.
- Gun shake at 5.6 h (frame-to-frame change in the gun's distance from the player): 2–10 units/s with curtimefix,
  330–380 without.
- Knife attack animation at 1.5 and 2.7 h: smooth with curtimefix; without it the animation barely advances evenly
  at all.
- On a freshly loaded map, the camera path is identical with curtimefix on, off or not loaded.

## Console

| Command | What it does |
|---|---|
| `curtimefix` | status: map time, float step, what was fixed |
| `curtimefix 0` / `1` | off / on, to compare in the same session |
| `curtimefix meter` | start, then run it again to stop and print how evenly the camera, gun and gun animation move |
| `curtimefix selftest` | runs the game's own path next to curtimefix's every frame. Stop it to get the summary: differences explained by the game's float times, differences across a teleport (both blend across it), and UNEXPLAINED ones; plus "no more changes" differences, where EARLY means curtimefix's only. Both bug counts should be 0. Prints up to 15 detailed reports for them |
| `curtimefix reset` | clear the counters |

Environment variables (Linux): `CURTIMEFIX_DISABLE=1` doesn't load it, `CURTIMEFIX_START_OFF=1` loads it switched
off, `CURTIMEFIX_TRACE=1` prints one line per frame to stderr.

## Limits

- Measured in game up to 5.6 h of map time. Exact sample times are recovered up to 131,072 s (36.4 h) at 66 and
  100 tick (`make test` checks it); past that the float step is longer than a tick, and samples are left as the
  game stored them, like stock CS:S.
- When the server's position history contains a teleport, the game blends across it for one tick (a short streak).
  Stock CS:S does the same; curtimefix doesn't change it.
- The 1.1 changes are shared code and the Windows build has them, but 1.1 hasn't been run on Windows yet. 1.0 was
  tested on Windows under Proton.

## Building

The code only needs a few Source SDK headers (for the `curtimefix` console command). Clone this repo somewhere
without spaces in the path, then get the headers:

```
git clone --filter=blob:none --sparse https://github.com/ValveSoftware/source-sdk-2013 third_party/source-sdk-2013
cd third_party/source-sdk-2013 && git checkout be57e3d01ac7d7aa6a2cacbfef187c0b46616db0
git sparse-checkout set src/common src/public src/tier1
```

**Linux:** `make && make check-glibc` in `linux/` builds `libcurtimefix.so`; `make test` checks the tick recovery. `check-glibc` makes sure it still
loads on the Steam runtime's glibc 2.31 and exports nothing.

**Windows** (cross-compiled on Linux): `win/build.sh` builds `curtimefix.dll` (clang-cl, MSVC ABI) and
`curtimefix.exe` (MinGW-w64) into `win/build/out/`. It needs clang, lld, mingw-w64-gcc and an
[xwin](https://github.com/Jake-Shadle/xwin) splat in `~/.local/share/xwin` (or set `XWIN=`).

## After a game update

`python3 tools/check_build.py` checks the installed Linux game. For Windows, pass the 64-bit pair:
`python3 tools/check_build.py <game>/cstrike/bin/x64/client.dll <game>/bin/x64/engine.dll`. It prints OK, or
which part moved.

## Files

| Path | What |
|---|---|
| `src/fix.cpp` | the fix, meter, self-test, commands |
| `src/find.cpp` | runtime discovery (RTTI, call sites, clock, gun slot) |
| `src/plat_linux.cpp`, `src/plat_win.cpp` | OS layer |
| `src/cmd.cpp` | the `curtimefix` console command (the only file using Source SDK headers) |
| `src/glibc_compat.cpp` | keeps the Linux build loadable on glibc 2.31 |
| `win/launcher.c` | `curtimefix.exe` |
| `tools/check_build.py` | the same discovery on files, for after updates |
| `tools/probe2/` | `probe2.so` (`make` there; `LD_PRELOAD` with `PROBE2_DIR=<dir>`) records the camera every frame and runs console commands appended to `<dir>/ctl`; `analyze.py`, `within_tick.py` and `yaw.py` read its `rec.bin` |

## Changes

**1.2:**
- Linux: curtimefix stayed off after a game restart through `execve`, which bhoptools does when it installs an
  update. The restarted game kept the "already loaded" mark in its environment, and its process name became `exe`.
  The mark now belongs to one exec, and the game is recognised by its binary.
- Past 65,536 s on 100-tick servers, samples stored after a prediction reset could be given a time one tick off.
  Sample times are now matched bit for bit against the two forms the game stores; when two ticks fit, the sample is
  left as stored. `tools/test_ticktime.cpp` checks this up to 39 h.
- Removed two unused debug switches.

**1.1** (found with `tools/probe2` and the reworked selftest on a dedicated server):
- On a dedicated server, the render-time half of the fix never ran. `gpGlobals->tickcount` there isn't the tick
  `curtime` was built from (one behind in a long session, up to two ahead after a map load), so the check in step 1
  failed on nearly every call and only your own predicted player was fixed. The tick now comes from `curtime` itself.
- `Interpolate` returns a "no more changes" flag that lets the game take an entity off its interpolation list. At
  a tick boundary curtimefix could report it one frame early, because the exact and the float target time land on
  different sides of a sample. It's now reported only where the game's own target time would report it.
- The selftest sorts every difference: float rounding, across a teleport, unexplained; early or harmless return values.

**1.0:** first release.

## Credits

curtimefix is by brybry333. `src/glibc_compat.cpp` is adapted from
[bhoptools](https://github.com/ddyfad/bhoptools) (GPL-3.0). `win/sdk-compat/` holds two Source SDK 2013 headers
with small changes for clang-cl.
