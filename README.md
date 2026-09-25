# HandTrackingValve

Full per-finger Valve Index hand tracking on Echo VR's chassis hands.

Echo animates fingers from three or four Touch inputs (trigger, grip, thumb
touch), so middle, ring and pinky always move together. The chassis skeleton
already has every finger joint (`EXP_L1_Index1..3`, `Middle1..3`, `Ring1..3`,
`Pinky1..3`, `Thumb1..3`, and the same for `R1`). This mod drives those joints
from the Index controllers' finger sensing.

## Pieces

| file | what it is |
| --- | --- |
| `HandTrackingValve.dll` | the plugin: hooks the game and poses the finger joints |
| `HandTrackingBridge.exe` | reads the Index finger curls from SteamVR and sends them to the plugin |
| `htv_actions.json`, `htv_bindings_knuckles.json` | the bridge's SteamVR input manifest; keep them next to the exe |
| `handtracking_config.txt` | settings; keep it next to the DLL |

**Why two processes:** inside `echovr.exe`, SteamVR input belongs to Revive. A
process gets one action manifest, and Revive's has no skeleton actions. The
bridge is its own SteamVR overlay app, with a manifest that asks for both hand
skeletons. It sends frames to the plugin over `127.0.0.1:8768` at about 120 Hz.

## Install and run

1. Run `build.bat`. The output goes to `out\`.
2. Put `HandTrackingValve.dll` and `handtracking_config.txt` in your plugin loader's folder.
3. Put `HandTrackingBridge.exe` and the two JSON files anywhere you like, in the same folder.
   The bridge loads `openvr_api.dll` from its own folder first, then Revive's
   `C:\Program Files\Revive\openvr_api64.dll`.
4. Start SteamVR, then run `HandTrackingBridge.exe --print`. The console shows live
   curls. Open and close each finger and check the numbers move.
5. Start Echo VR. Hold your hands **fully open** once: that captures the reference
   pose (the game's relaxed pose becomes "curl = 0").

`HandTrackingValve.log` next to the DLL records the hook, each calibration, the
detected finger order, and the list of hand-animator instances.

### Commands

```
HandTrackingBridge.exe --print                  stream and show curls
HandTrackingBridge.exe --calibrate              re-capture the open-hand reference
HandTrackingBridge.exe --set "BendAxis = x"     change a setting live
HandTrackingBridge.exe --ping                   check the plugin is loaded
```

Editing `handtracking_config.txt` while the game runs also works; it is re-read
within half a second.

## First-run tuning

These are the parts that can only be found in-game:

| symptom | fix |
| --- | --- |
| fingers don't move at all | check the log for `hooked` and `receiving tracking frames`; then `TargetInstance` (see below) |
| someone else's hands move | the log lists instances; set `TargetInstance` to yours |
| fingers bend backwards | flip `LeftBendSign` / `RightBendSign` |
| fingers bend sideways or twist | change `BendAxis` (x/y/z) |
| left and right swapped | `GameHandLeft = 1` |
| wrong finger moves | set `FingerOrder` explicitly, e.g. `index,middle,ring,pinky,thumb` |
| thumb wrong | `ThumbBendAxis`, `LeftThumbSign` / `RightThumbSign` |

## How it works

Echo runs its frame as a table of gamespace tasks. Two belong to `CR15HandAnimatorCS`,
back to back:

| phase | task | address |
| --- | --- | --- |
| `0x6008` | `UpdateFingerAnimPoses` | `echovr+0x99f440` |
| `0x6009` | `UpdateThumbPistonAnimPoses` | `echovr+0x9a16e0`, **hooked** |

`UpdateFingerAnimPoses` only queues jobs (`echovr+0x99f830`, one per finger) for a
hand whose **contact** weight is non-zero. Those jobs wrap the fingers around
whatever the hand is touching. A hand touching nothing gets no jobs. So at
`0x6009` the animation for that hand is already written and nothing else is
still working on it. The hook poses those hands and then calls the original.
Hands in contact are left to the engine (`RespectContact = 1`).

Joints are read and written with the engine's own accessors:

- `get` `echovr+0x331bd0` returns `{quat xyzw, pos, scale}`.
- `set` `echovr+0x37c8c0` writes a joint **and carries its children along**.

Each finger is set proximal → middle → distal. Every joint gets its calibrated
local rotation with a bend of `curl × MaxCurlN` degrees about the configured
axis, so the game's grip animation is **replaced**, not stacked on top.

Offsets are for the single live build
(`C:\Oculus\Games\Software\Software\ready-at-dawn-echo-arena\bin\win10\echovr.exe`).
All three prologues are checked before hooking, and any mismatch leaves the game
untouched. Any fault while posing turns the plugin off for the session rather
than crashing.

## Known limits

- **Local only.** Other players see your fingers the way the game networks them (a
  compact finger state), not the tracked pose.
- **Grabbing still uses the game's own grip input.** This changes what the hand
  looks like, not what it does.
- **Not yet tested in-game.** The hook point, the joint accessors and the struct
  offsets come from disassembling the live build. The bend axis and sign, which
  instance is you, and whether SteamVR sends skeletal input to an overlay app
  while Echo has focus have not been seen working. The table above covers the
  first two. For the last one, check that `--print` values keep moving while the
  game is focused.
