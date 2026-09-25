<img src="installer/logo/echoxr.png" width="96" alt="EchoXR logo">

# EchoXR

EchoXR runs **Echo VR on SteamVR through OpenXR**, with no Oculus app in the
path, and adds **EchoXR Hands**: per-finger Valve Index hand tracking on Echo's
chassis hands. The two parts are independent. EchoXR works with any headset
SteamVR supports. EchoXR Hands works with or without EchoXR, as long as Echo
loads plugins (see [Plugin loader](#plugin-loader)).

EchoXR Hands used to be called HandTrackingValve. The installer and the launcher
both migrate old installs.

## What it does

| | |
| --- | --- |
| **Echo on SteamVR** | Echo's Oculus calls (LibOVR) are answered by ReviveXR over OpenXR, pinned to SteamVR for each launch. Details: [xr/README.md](xr/README.md). |
| **Your own fingers** | Every finger bends on its own, from the Index controllers' finger sensing. Echo normally moves middle, ring and pinky together from the grip button. |
| **Other players' fingers** | The plugin sends your finger curls through a relay (`Network = 1`), and poses players who send theirs. |
| **Auto-start** | `EchoXR.exe` can run the finger bridge alongside Echo and close it afterwards. |
| **Install and update** | `EchoXRSetup.exe` is an installer with a GUI. The release zip needs nothing more than unzipping; `EchoXR.exe` does the setup on first launch. |

### What has been seen working

From the logs of real sessions on the current Echo build:

- **EchoXR:** Echo starts and runs a full session on SteamVR/OpenXR 2.17.10, with
  zero failed OpenXR calls.
- **EchoXR Hands:** the plugin hooks the game and finds the local player in a
  match. It receives the bridge's frames at about 120 Hz and calibrates.
- **Relay:** the plugin connects to the relay and sends your fingers.

### Not confirmed yet

- **Pose tuning.** The bend axis and direction are worked out from the rig
  automatically. If a finger bends the wrong way, the settings below fix it; see
  [First-run tuning](#first-run-tuning).

### Limits

- **It changes how hands look, not what they do.** Grabbing still uses the game's
  own grip input.
- **Other players see your tracked fingers only if they run the plugin.**
  Everyone else sees the game's normal finger animation.
- **One game build.** The patch for `echovr_openxr.exe` and the plugin's hooks
  are for the current `echovr.exe` (35,397,120 bytes, May 2023). Both check the
  bytes they change first, and refuse anything else instead of breaking the game.
- **The finger bridge needs SteamVR and Valve Index controllers.** Without an Index
  you can still see other players' fingers; `fake_index.py` sends test input.

## Install

### Release zip

`EchoXR-v<version>.zip` unpacks into Echo's `bin\win10` folder, the one with
`echovr.exe`:

```
bin\win10\
  EchoXR.exe                  the launcher
  EchoXR\                     OpenXR runtime, loader, licences, README.txt
    Hands\                    finger bridge (EchoXRHands.exe) and its SteamVR files
      install\                plugin, default settings, plugin loader
```

Run `EchoXR.exe` with SteamVR installed. On each launch it sets up whatever is
missing or out of date, then starts Echo:

- **`echovr_openxr.exe`:** a patched copy of `echovr.exe`, made on the player's
  machine. No game file ships in the zip.
- **Plugin loader:** installed following the [loader rules](#plugin-loader).
- **Hand tracking plugin:** `plugins\EchoXRHands.dll` is copied in, or updated. The
  old `HandTrackingValve.dll` is removed and its settings are kept.
- **`EchoXR\echoxr.ini`:** created with `AutoStartHands = 1`. The zip doesn't ship
  one, so unzipping a newer release never resets your choice.

`EchoXR.exe --setup-only` does the setup without starting Echo.

### Installer

`EchoXRSetup.exe` finds Echo on its own, or you can browse to it. It shows each
part as a switch:

| switch | what it installs |
| --- | --- |
| Hand tracking | `plugins\EchoXRHands.dll` and `plugins\EchoXRHands.txt`. An existing `EchoXRHands.txt` is kept; new defaults go to `EchoXRHands.default.txt`. |
| Finger bridge | `EchoXR\Hands\`: `EchoXRHands.exe`, its SteamVR manifest, `openvr_api.dll`, `fake_index.py` |
| EchoXR runtime | `EchoXR.exe`, `EchoXR\` (runtime, OpenXR loader, licences) and the patched `echovr_openxr.exe` |
| Start hand tracking with EchoXR | `EchoXR\echoxr.ini` `AutoStartHands = 1` or `0` |
| Plugin loader | `dbgcore.dll`, following the [loader rules](#plugin-loader) |
| Desktop shortcuts | `EchoXR.lnk` and `EchoXR Hands.lnk` |

The installer also clears out pre-rename files. It removes
`plugins\HandTrackingValve.dll` and the `HandTrackingBridge\` folder, and renames
`handtracking_config.txt` to `EchoXRHands.txt`. Old desktop shortcuts into that
install are replaced with the new ones. It warns if Echo or the bridge is running,
and offers to rerun as administrator if Windows blocks the folder.

Uninstall removes the switched-on parts. It keeps `EchoXRHands.txt`, and it keeps
the loader unless the previous `dbgcore.dll` can be put back.

To run it without the window:

```
EchoXRSetup.exe --silent [--dir <folder>] [--components <mask>] [--uninstall]
```

The mask bits are 1 hand tracking, 2 bridge, 4 EchoXR, 8 loader, 16 shortcuts and
32 auto-start. The log is `%TEMP%\EchoXRSetup.log`.

### Plugin loader

The plugin is loaded by `dbgcore.dll`, a plugin loader that loads every DLL in
`bin\win10\plugins\`. The zip and the installer apply the same rules
(`xr/src/echoxr_common.h`):

| found in `bin\win10` | what happens |
| --- | --- |
| no `dbgcore.dll` | the loader is installed |
| the older 45 KB `dbgcore.dll` | the loader is installed, and the old file moves to `plugins\dbgcore_legacy.dll`, where it still loads |
| any other `dbgcore.dll`, with a `plugins\` folder | kept, since it's already a plugin loader; only the plugin is added |
| any other `dbgcore.dll`, no `plugins\` folder | left alone. The zip skips hand tracking. The installer replaces it only when you switch Plugin loader on, and saves the old file as `dbgcore.dll.bak`. |

### Linux (untested)

The release zip also runs on Linux through Proton. It uses the Proton that Steam
already has, and Steam's Linux runtime container when that's installed. There's
no other app to install.

1. Copy the whole `ready-at-dawn-echo-arena` folder from a Windows PC.
2. Unzip the release into its `bin/win10` folder.
3. Copy `LibOVRPlatform64_1.dll` and `LibOVRPlatformImpl64_1.dll` from the Windows
   PC's `C:\Program Files\Oculus\Support\oculus-runtime\` into `bin/win10`.
   `pnsovr.dll` needs them to log in, and a Linux prefix has no Oculus folder.
4. Set an active OpenXR runtime (SteamVR, Monado or WiVRn), then run
   `bin/win10/EchoXR/echoxr-linux.sh`.

The VR path stays inside the game process:

```
echovr_openxr.exe -> EchoXR\LibOVRRT64_1.dll -> EchoXR\openxr_loader.dll
  -> Proton's wineopenxr.dll (the prefix's registered OpenXR runtime)
  -> wineopenxr.so -> the Linux runtime in XR_RUNTIME_JSON
```

Under Wine, `EchoXR.exe` notices `wine_get_version` and leaves the runtime
choice to Proton instead of pinning SteamVR's Windows manifest. The script:

- **Picks Proton:** the newest GE-Proton, then Proton Experimental, then the
  newest `Proton N`. It refuses one without `wineopenxr`.
- **Picks the runtime:** `XR_RUNTIME_JSON`, or your active one in
  `~/.config/openxr/1/active_runtime.json`.
- **Uses its own prefix:** `~/.local/share/echoxr/prefix`.
- **Sets Proton up:** the `STEAM_COMPAT_*` variables, plus
  `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1` so the container can see the
  runtime.
- **Keeps the plugin loader:** `WINEDLLOVERRIDES=dbgcore=n,b`, because Wine
  would otherwise load its own `dbgcore.dll` instead of the plugin loader.

`ECHOXR_PROTON`, `ECHOXR_PREFIX`, `ECHOXR_NO_CONTAINER=1` and `ECHOXR_DEBUG=1`
override the choices. `ECHOXR_DEBUG=1` also writes Proton and OpenXR loader logs.

The approach follows [RiftLift](https://github.com/Villagers654/RiftLift), which
runs Rift games the same way. No RiftLift code is used: it's GPL-3.0.

Nothing here has been run on Linux yet. The open questions are:

- whether Echo's renderer works with Proton's `wineopenxr` (the Windows logs show
  a D3D12 device);
- whether `pnsovr.dll` and the Platform SDK DLLs log in under Wine;
- whether the finger bridge (`EchoXRHands.exe`, OpenVR) works. That needs SteamVR,
  because Monado and WiVRn don't provide OpenVR.

## Using it

1. Start SteamVR.
2. Run `EchoXR.exe` (or the EchoXR desktop shortcut). With auto-start on, the
   finger bridge opens minimised next to Echo. Without it, run
   `EchoXR\Hands\EchoXRHands.exe --print` yourself.
3. In-game, hold both hands **fully open** once. That captures the reference pose:
   the game's relaxed pose becomes "curl = 0".

The bridge also takes commands:

```
EchoXRHands.exe --print                  stream and show the live curls
EchoXRHands.exe --calibrate              re-capture the open-hand reference
EchoXRHands.exe --set "BendAxis = x"     change a setting live
EchoXRHands.exe --ping                   check the plugin is loaded
```

Settings are in `plugins\EchoXRHands.txt`. The plugin re-reads it within half a
second, so you can edit it while you play.

`Platform` and `PlatformAccount` are experimental, and empty (off) by default.
They change the platform prefix and account number of your login ID in memory at
startup (`pnsovr.dll`), and nothing on disk changes. Leave them off on EchoVRCE:
its server numbers platforms differently from the game, and it rejects the result.

### Why a separate bridge

Inside the game, SteamVR input belongs to the runtime, and a process gets exactly
one SteamVR action manifest. That manifest has no hand-skeleton actions. The bridge
is its own SteamVR overlay app, with a manifest that asks for both hand skeletons.
It sends frames to the plugin over `127.0.0.1:8768`.

### Finger sharing and privacy

With `Network = 1` (the default), the plugin connects to
`RelayUrl` (`wss://sparkapi-production-e6df.up.railway.app/htv/ws`). It sends:

- your finger curls, 30 times a second (`NetSendHz`);
- your display name;
- the names of the players in your match, so the relay can pair you with other
  players running the plugin.

The relay doesn't store anything. `Network = 0` keeps your tracking to yourself.

## First-run tuning

| symptom | fix |
| --- | --- |
| fingers don't move at all | check `plugins\EchoXRHands.log` for `hooked` and `receiving tracking frames`; then `TargetInstance` |
| someone else's hands move | the log lists instances; set `TargetInstance` to yours |
| fingers bend backwards | flip `LeftBendSign` / `RightBendSign` |
| fingers bend sideways or twist | set `BendAxis` to `x`, `y` or `z` (default `auto`) |
| left and right swapped | `GameHandLeft = 1` |
| wrong finger moves | set `FingerOrder` explicitly, e.g. `index,middle,ring,pinky,thumb` |
| thumb wrong | `ThumbBendAxis`, `LeftThumbSign` / `RightThumbSign` |
| shaky / laggy | lower `FilterMinCutoff` / raise `FilterBeta` |

## Logs

| log | what's in it |
| --- | --- |
| `plugins\EchoXRHands.log` | hooks, calibrations, finger order, hand-animator instances, relay status every 5 s |
| `EchoXR\launcher.log` | first-run setup, the runtime chosen, the bridge starting and stopping, Echo's exit code |
| `EchoXR\runtime.log` | the OpenXR runtime and its extensions, and every failed OpenXR call |
| `%TEMP%\EchoXRSetup.log` | what the installer wrote, kept, moved or removed |

## How the hand tracking works

Echo runs its frame as a table of gamespace tasks. Two belong to
`CR15HandAnimatorCS`, back to back:

| phase | task | address |
| --- | --- | --- |
| `0x6008` | `UpdateFingerAnimPoses` | `echovr+0x99f440` |
| `0x6009` | `UpdateThumbPistonAnimPoses` | `echovr+0x9a16e0`, **hooked** |

`UpdateFingerAnimPoses` only queues jobs (`echovr+0x99f830`, one per finger) for a
hand whose **contact** weight is non-zero. Those jobs wrap the fingers around
whatever the hand is touching. A hand touching nothing gets no jobs, so at
`0x6009` its animation is already written and nothing else is still working on it.
The hook poses those hands and then calls the original. Hands in contact are left
to the engine (`RespectContact = 1`).

Joints are read and written with the engine's own accessors:

- `get` `echovr+0x331bd0` returns `{quat xyzw, pos, scale}`.
- `set` `echovr+0x37c8c0` writes a joint **and carries its children along**.

Each finger is set proximal → middle → distal. Every joint gets its rest rotation
from the chassis rig (`UseRig = 1`), with a bend of `curl × MaxCurlN` degrees. The
game's grip animation is therefore **replaced**, not stacked on top.

Other players' fingers come in through the relay. They're applied in
`CR15RemotePlayerCS::UpdateCachePoseForPhysics` (`echovr+0xd77be0`, phase
`0x600c`), and `CR15NetGame::Update` finds who is in the match.

Every hooked function's first bytes are checked before hooking, and any mismatch
leaves the game untouched. A fault while posing turns the plugin off for the
session rather than crashing the game.

## Building

Everything builds with MSVC (Visual Studio 2026 toolset).

| command | builds |
| --- | --- |
| `build.bat` | `out\EchoXRHands.dll` (plugin), `out\EchoXRHands.exe` (bridge), settings and manifests |
| `xr\build_xr.bat` | `xr\out\LibOVRRT64_1.dll`, `openxr_loader.dll` and `EchoXR.exe`. It needs three upstream checkouts plus a patch; [xr/README.md](xr/README.md) has the exact commits and commands |
| `xr\build_launcher.bat` | just `xr\out\EchoXR.exe` (quick) |
| `installer\build_installer.bat [--all]` | all of the above as needed, then `out\EchoXRSetup.exe` |
| `python tools\make_release.py [--no-build]` | `out\release\EchoXR-v<VERSION>.zip` and `EchoXRSetup-v<VERSION>.exe` |
| `python tools\gen_logo.py` | the logo in `installer\logo\` (SVG, PNG, ICO) |

The plugin loader is staged from the game install into
`installer\stage\dbgcore.dll` when the installer is built. The version number is
in `VERSION`. `linux\echoxr-linux.sh` needs no build; the release zip ships it as
`EchoXR/echoxr-linux.sh`.

| folder | what |
| --- | --- |
| `plugin/` | EchoXR Hands plugin (hooks, posing, relay, Platform setting) |
| `bridge/` | finger bridge (SteamVR input → plugin, UDP `127.0.0.1:8768`) |
| `xr/` | EchoXR runtime glue and launcher (`src/`), Revive patch (`patches/`) |
| `installer/` | `EchoXRSetup.exe` source, logo |
| `linux/` | Linux launcher script |
| `tools/` | release packaging, logo generator, rig table generator, `fake_index.py` |
