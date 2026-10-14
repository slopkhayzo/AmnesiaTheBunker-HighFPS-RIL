# AmnesiaTheBunker.FPSFix

> [!WARNING]
> This patch has been pretty much entirely been generated using AI; 
> I won't and will never claim to have enough knowledge or expertise to do this 
> kind of reverse-engineering by myself; I've tested till Maintenance and the Prison at 360hz + 
> briefly tested at 120, 144 and 240 and so far spotted 
> no major issues (on my machine ofc, if you have issues feel free to open an Issue).
> If you're interested and want more (human generated) info, I have a blog post [here](https://slop-blog.enkhayzomachines.net/posts/amnesia-the-bunker-high-fps-ril) :)

High-frame-rate presentation fix for the 64-bit Windows build of
*Amnesia: The Bunker*, delivered as an ASI plugin.

The game simulates at the rate of its Framerate option (120 at most) and
normally draws at the same rate. The fix lets the game draw faster than it
simulates and presents each frame between the two most recent simulation
states, so motion is smooth at the display's rate while physics, scripts, AI,
animation timing, and audio are unchanged. Everything it changes for a frame
is restored before the simulation runs again.

## What `1.0.0` covers

- Render unlock: the engine's own `LimitFPS` gate is cleared so drawing is no
  longer tied to the gameplay update rate.
- An independent presentation limiter, by default at the primary display's
  refresh rate, with optional frame pacing (`FramePacingEvenness`) that evens
  out frame times around each simulation step at some cost in frame rate.
- Camera interpolation on the current listener camera, including scripted
  cameras.
- Display-rate mouse look: motion is previewed on the rendered camera every
  frame and delivered to the game exactly once per tick.
- First-person body root and skeleton (arms, hands, held-item rigs), weapons,
  the lantern, and attached equipment.
- World objects: props, doors, drawers, thrown and carried objects, lights,
  and their shadows.
- Skeletal animation of creatures, characters, and animated props.
- Particle positions (smoke, sparks, fire, dust).
- Menu, pause-menu, and inventory cursors, including dragged items.

Not implemented: ragdoll poses, time-based screen effects (image trails,
blur), ropes, beams, light glows, ambient dust, particle colour and size,
particles attached to carried objects, and the cursor on in-world terminal
screens. These still update at the simulation rate.

The shipped INI holds the simulation at 60 Hz (`GameplayRateOverride = 60`),
which is how the fix was developed and validated. Set it to `0` to keep the
rate chosen by the game's Framerate option; at 120 Hz a simulation step leaves
room for only one or two frames, so the frame rate then stays between 120 and
240.

## Compatibility

Developed and tested on the GOG build only (`AmnesiaTheBunker.exe`, SHA-256
`377664BA3E6FC84D4F9B04A75A1534868467106E7135FA951167E2C223C2DBD5`). The
executable hash is logged but does not gate the fix: on any other x64
executable the known mappings are marked unverified, and each one is used only
if the code at its address matches its signature. Hooks are installed
transactionally, so a mismatch switches the affected feature (or the whole fix)
off and records the first failure in the log. Other builds, such as Steam, work
only if their code is identical; this is untested.

The game has not been played to the end with this version, and gamepads, mods,
and custom stories are untested.

The private `research/` directory (reverse-engineering notes, validation logs,
the working plan) is intentionally excluded from Git.

## Building

Use an x64 MSVC environment with CMake 3.24 or newer:

```powershell
cmake -S . -B build-x64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-x64 --parallel
ctest --test-dir build-x64 --output-on-failure
```

The ASI and the development INI are emitted under `build-x64/bin`.

Set `ATB_GAME_EXE` while configuring to add a read-only test that maps an
installed executable as an image and validates its identity and signatures:

```powershell
cmake -S . -B build-x64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DATB_GAME_EXE="D:/Program Files/Games/GOG/Amnesia The Bunker/AmnesiaTheBunker.exe"
```

## Configuration files

- [packaging/AmnesiaTheBunker.FPSFix.ini](packaging/AmnesiaTheBunker.FPSFix.ini)
  is the release INI, with every validated feature on and each setting
  documented inline. The read-only telemetry layers that enabled features need
  are switched on implicitly, so it has no `[Telemetry]` section.
- [config/AmnesiaTheBunker.FPSFix.ini](config/AmnesiaTheBunker.FPSFix.ini) is
  the development default, with everything off. It is copied beside the
  plugin on every build and is what the tests stage.
- The other files in [config/](config/) are development profiles (telemetry,
  traces, probes, per-layer interpolation) used by the validation runners.

`RagdollPoseInterpolation` and `ImageTrailTiming` are not implemented; setting
either to `true` rejects the whole configuration. Invalid or unsupported
feature combinations are likewise rejected before any hook is installed.

## Packaging

```powershell
cmake --build build-x64 --target package_plugin_only
cmake --build build-x64 --target package_loader_bundled
```

These write two flat archives to `dist/`:

- `AmnesiaTheBunker.FPSFix-<version>-plugin-only.zip`
- `AmnesiaTheBunker.FPSFix-<version>-with-Ultimate-ASI-Loader.zip`

The second target exists only when CMake is configured with
`ATB_ULTIMATE_ASI_LOADER_DLL` pointing at the official x64 Ultimate ASI Loader
`v9.7.4` `version.dll`; its hash is checked against the value in
[third_party/ultimate-asi-loader/UPSTREAM.md](third_party/ultimate-asi-loader/UPSTREAM.md).
The game does not import `version.dll` itself, but Windows loads it from the
game folder through system libraries that do. Both archives carry the same ASI
and INI, the end-user README from [packaging/](packaging/), the applicable
licenses, and `SHA256SUMS.txt`.

## Development runs

`atb_dev_launcher.exe` (built unless `ATB_BUILD_DEV_LAUNCHER=OFF`) starts the
game as a new process and injects the plugin, without installing anything into
the game folder. It is never included in release archives:

```powershell
cmake --build build-x64 --target stage_telemetry_profile
.\build-x64\bin\atb_dev_launcher.exe `
  "D:\Program Files\Games\GOG\Amnesia The Bunker\AmnesiaTheBunker.exe" `
  "$PWD\build-x64\bin\AmnesiaTheBunker.FPSFix.asi"
```

The scripts in [tools/](tools/) (`run_*_validation.cmd`, `run_play.cmd`) stage
a profile from [config/](config/), back up the game's user settings, launch the
game through the launcher, and either restore or leave the staged settings
depending on the runner. [tools/analyze_frames.py](tools/analyze_frames.py)
summarises the binary frame traces written by `[Telemetry] FrameTrace`, and
compares a play run against a `run_baseline.cmd` run.

## Logs

`AmnesiaTheBunker.FPSFix.log` is written beside the ASI (or under
`%LOCALAPPDATA%\AmnesiaTheBunker.FPSFix` if that is not writable) and replaced
on each launch, with the previous session kept as
`AmnesiaTheBunker.FPSFix.previous.log`. With `Diagnostics = true` each layer
reports its counters every five seconds.

## Third-party

- [MinHook](third_party/minhook/) for function hooking.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
  (binary not stored in Git) for the loader-bundled archive.
