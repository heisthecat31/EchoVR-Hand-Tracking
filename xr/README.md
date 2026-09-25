# EchoXR — Echo VR on SteamVR through OpenXR

EchoXR runs Echo VR on SteamVR, with no Oculus runtime in the path. Echo was
written against Oculus's LibOVR API. EchoXR answers those calls with an OpenXR
implementation, so the game renders and tracks through SteamVR's OpenXR runtime,
or any other OpenXR runtime you choose.

It ships as part of the EchoXR release zip and `EchoXRSetup.exe` (see the
[top-level README](../README.md)).

---

## The patched game executable

Echo's built-in Oculus loader only accepts an Oculus-signed runtime, and the
EchoXR runtime isn't signed by Oculus. EchoXR therefore runs Echo from
`echovr_openxr.exe`, a copy of `echovr.exe` in which the LibOVR runtime signature
check always passes. The function at RVA `0x1365bd0` starts with
`mov eax,1 ; ret` (6 bytes) instead of its prologue. `echovr.exe` itself is
never changed, so normal launches are unaffected.

The copy is made on the player's own machine: by `EchoXR.exe` on its first
launch, or by `EchoXRSetup.exe` as part of the EchoXR component. Both use
`src/echoxr_common.h`, which first checks the 14 prologue bytes
(`48 89 5C 24 18 48 89 74 24 20 55 57 41 56`); on any other game build it refuses
and leaves nothing behind. No game binary ships with this project. Uninstalling
EchoXR deletes the copy.

---

## What each part is

| File | Where it goes | What it does |
| --- | --- | --- |
| `LibOVRRT64_1.dll` | `bin\win10\EchoXR\` | The runtime. Revive's OpenXR backend ("ReviveXR", MIT), built under the Oculus runtime's DLL name. It implements every one of the 93 `ovr_*` functions Echo uses (125 in total) on top of OpenXR. |
| `openxr_loader.dll` | `bin\win10\EchoXR\` | The official Khronos OpenXR loader, built from source (OpenXR SDK 1.1.63). |
| `EchoXR.exe` | `bin\win10\` | The launcher, described below. |
| `echovr_openxr.exe` | `bin\win10\` | The patched copy of `echovr.exe`, made on your machine by the launcher or the installer (see above). |

The runtime lives in its own `EchoXR\` subfolder on purpose. Echo searches its
own folder first for its runtime, so a runtime placed there would take over
normal launches too. Kept in the subfolder, it's only used when the launcher
points Echo at it. Starting `echovr.exe` the usual way is unaffected.

### The launcher, `EchoXR.exe`

Source: `src/launcher.cpp`. It sets up one launch, starts Echo and waits for it
to exit:

1. **Checks and sets up.** `EchoXR.exe` has to be next to `echovr.exe`, and
   `EchoXR\LibOVRRT64_1.dll` has to exist; otherwise it says so in a message box.
   If `echovr_openxr.exe` is missing, it makes it (see above). From a release
   zip, it also installs or updates the hand tracking plugin and plugin loader from
   `EchoXR\Hands\install\`, following the same loader rules as the installer.
   `--setup-only` stops here.
2. **Chooses the VR runtime.** It finds SteamVR through Steam's own registry
   (`%LOCALAPPDATA%\openvr\openvrpaths.vrpath`). It then points this launch at
   SteamVR's `steamxr_win64.json` via the loader's `XR_RUNTIME_JSON` variable.
   This matters because some apps (the Virtual Desktop streamer, for one) keep
   making themselves the system-wide OpenXR runtime. `--runtime active` uses the
   system runtime instead. Under Wine/Proton (it checks for `wine_get_version`) it
   leaves the choice to Proton, whose registered runtime is `wineopenxr`; see
   "Linux" in the top-level README.
3. **Tells Echo a headset is present.** Echo checks for the named Windows event
   `OculusHMDConnected` before it starts VR; normally the Oculus service creates
   it. The launcher creates it when nothing else has.
4. **Points Echo at the runtime folder.** It sets `LIBOVR_DLL_DIR` to
   `bin\win10\EchoXR\`, the first place Echo's loader looks. It also adds that
   folder to `PATH` so `openxr_loader.dll` is found.
5. **Starts Echo.** That's `echovr_openxr.exe` by default, or `--exe <name>` for
   another executable in the same folder. Any other arguments are passed to Echo.
6. **Starts hand tracking**, if `EchoXR\echoxr.ini` has `AutoStartHands = 1` (the
   installer asks; without an ini, the launcher creates one with `1`). It runs `EchoXR\Hands\EchoXRHands.exe --print` in its own
   minimised console for as long as Echo runs, restarts it if it exits (at most 20
   times, 5 seconds apart), and closes it when Echo exits. If the bridge is already
   running, the launcher leaves it alone.

The environment settings apply only to the Echo process the launcher starts;
nothing system-wide is changed. It logs to `bin\win10\EchoXR\launcher.log`.

---

## Setting up a PC

1. **Install SteamVR** (free, in Steam) and check that your headset shows up in
   it. It doesn't have to be set as the system OpenXR runtime; the launcher picks
   SteamVR for each launch on its own.
2. **Unzip the release** into the Echo install's `bin\win10` folder, so that
   `EchoXR.exe` sits next to `echovr.exe`, or run `EchoXRSetup.exe`.
3. **Run** `bin\win10\EchoXR.exe` with SteamVR running.

The Oculus login/platform side is separate from all of this. Echo's online
features on the community servers use the community platform files, the same as
with any other launch.

---

## Building

Everything builds with MSVC (Visual Studio 2026 toolset) from `build_xr.bat`.

Inputs, all fetched as source. The three checkouts aren't in this repository (they're
in `.gitignore`); these are the exact commits the build uses:

| Folder | What | Source | Commit |
| --- | --- | --- | --- |
| `Revive/` | Revive (MIT), with its submodules, plus `patches/revive-echoxr.patch` | github.com/LibreVR/Revive | `ab73167` |
| `ovr_sdk_pc/` | Oculus PC SDK **1.55** headers and shim sources | github.com/sparsebase/ovr_sdk_win | `4b1d6b7` |
| `OpenXR-SDK/` | OpenXR SDK 1.1.63 | github.com/KhronosGroup/OpenXR-SDK | `f2448a8` |
| `openxr-build/` | the OpenXR loader, built with CMake + Ninja | built from `OpenXR-SDK/` | — |

From the `xr` folder:

```
git clone --recursive https://github.com/LibreVR/Revive Revive
git -C Revive checkout ab73167e2380135aee9fe9f68874ec7dd2912cb0
git -C Revive submodule update --init --recursive
git -C Revive apply ../patches/revive-echoxr.patch
git clone https://github.com/sparsebase/ovr_sdk_win ovr_sdk_pc
git -C ovr_sdk_pc checkout 4b1d6b7e86077062e5de33dda7d77b5790202c4c
git clone https://github.com/KhronosGroup/OpenXR-SDK OpenXR-SDK
git -C OpenXR-SDK checkout f2448a8797c85814aa892efc1ab8707900fbcc78
```

`patches/revive-echoxr.patch` holds the three SteamVR changes described below, plus
the `runtime.log` hook in `Common.h`.

The Oculus SDK has to be **1.55 or newer**. Echo asks for SDK minor version 55,
and 1.43 is missing `ovrHmdColorDesc` and the separate audio-device error codes
that Revive uses.

`build_xr.bat` compiles ReviveXR's sources, with Revive's own `main.cpp` swapped
for `src/xr_main.cpp`, into `out\LibOVRRT64_1.dll`, and copies
`openxr_loader.dll` next to it. The launcher builds from `src/launcher.cpp` into
`out\EchoXR.exe` (`build_launcher.bat`, which `build_xr.bat` calls).

### What `src/xr_main.cpp` replaces

Revive normally runs as an injection layer: its `main.cpp` hooks `LoadLibrary`
so a game asking for the Oculus runtime gets Revive instead. EchoXR doesn't do
that. The runtime is loaded under the Oculus runtime's own name, from a folder
the launcher points Echo at. So `xr_main.cpp` is a plain `DllMain`. It also
writes `EchoXR\runtime.log` (see below).

---

## Changes made to Revive for SteamVR

Revive had mostly been used with Oculus-style runtimes. SteamVR rejected three
things. Each was found from `runtime.log`, which names the exact OpenXR call that
failed, and fixed in this copy of Revive:

1. **OpenXR version.** Built against the 1.1 SDK, Revive asked for OpenXR 1.1.
   The runtime refused (`XR_ERROR_API_VERSION_UNSUPPORTED`), which Echo reported
   as "Failed to initialize Oculus VR session" (`-3004`). It now asks for
   OpenXR 1.0 (`Runtime.cpp`).
2. **A struct from an extension that wasn't enabled.** Revive always attached an
   `XR_EPIC_view_configuration_fov` struct when reading view configurations.
   SteamVR doesn't offer that extension and validates strictly, so the call
   failed with `XR_ERROR_VALIDATION_FAILURE`. The struct is now only attached
   when the extension is enabled (`Session.cpp`).
3. **Reading the field of view too early.** SteamVR only answers `xrLocateViews`
   once a session is running. SteamVR now gets the same "wait until the session
   is ready" handling Revive already used for Windows Mixed Reality, with a
   5-second limit so an asleep headset can't hang Echo's startup. Revive's
   Echo-specific setting now also applies to the `echovr_openxr.exe` filename
   (`Runtime.cpp`, `Session.cpp`).

Result on SteamVR/OpenXR 2.17.10: Echo runs, with **no** failed OpenXR calls for
the whole session.

---

## Logs

| Log | What's in it |
| --- | --- |
| `bin\win10\EchoXR\launcher.log` | which runtime was chosen, what was launched, Echo's exit code |
| `bin\win10\EchoXR\runtime.log` | the OpenXR runtime's name and version, enabled extensions, the SDK version Echo asked for, and **every failed OpenXR call** with its source line |
| `_local\r14logs\*.log` | Echo's own log ("Initializing OVR session…" and any session error) |
| `Steam\logs\vrserver.txt` | SteamVR's side of the connection |

---

## Credits and licences

- **Revive / ReviveXR** — LibreVR, MIT License.
- **OpenXR SDK and loader** — The Khronos Group, Apache 2.0.
- **Oculus PC SDK headers** — Oculus/Meta. Used at build time only.
- **Microsoft Detours** — MIT License. Used by ReviveXR's D3D code.
