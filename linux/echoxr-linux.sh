#!/usr/bin/env bash
# EchoXR on Linux -- runs Echo VR through Proton, on your Linux OpenXR runtime
# (SteamVR, Monado, WiVRn, ...). Nothing extra to install: it uses the Proton that
# Steam already has, and Steam's own Linux runtime container when it's there.
#
#   echoxr-linux.sh [--dir <Echo bin/win10>] [--setup-only] [Echo arguments...]
#
# How the VR path works, all inside the one game process:
#   echovr_openxr.exe -> EchoXR\LibOVRRT64_1.dll (Oculus API over OpenXR)
#     -> EchoXR\openxr_loader.dll -> Proton's wineopenxr.dll (registered in the prefix)
#     -> wineopenxr.so -> the Linux runtime named by XR_RUNTIME_JSON
#
# Settings (environment):
#   ECHOXR_PROTON=/path/to/proton-dir   pick a Proton (default: newest GE-Proton, then Proton Experimental)
#   ECHOXR_PREFIX=/path                 Wine prefix (default: ~/.local/share/echoxr/prefix)
#   ECHOXR_NO_CONTAINER=1               run Proton directly, without Steam Linux Runtime
#   ECHOXR_DEBUG=1                      Proton + OpenXR loader logs (in ~/.local/share/echoxr/logs)
#   XR_RUNTIME_JSON=/path/manifest.json OpenXR runtime (default: your active runtime)
set -euo pipefail

die() { echo "echoxr: $*" >&2; exit 1; }
say() { echo "echoxr: $*" >&2; }

# ---- the Echo install: bin/win10 with echovr.exe and EchoXR.exe ---------------------
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo_dir=""
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --dir) echo_dir="${2:-}"; shift 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
if [ -z "$echo_dir" ]; then
    # the release zip puts this script in bin/win10/EchoXR/
    for d in "$here/.." "$here" "$PWD"; do
        if [ -f "$d/echovr.exe" ]; then echo_dir="$d"; break; fi
    done
fi
[ -n "$echo_dir" ] || die "can't find Echo VR -- run from bin/win10/EchoXR/, or pass --dir <.../ready-at-dawn-echo-arena/bin/win10>"
[ -d "$echo_dir/bin/win10" ] && echo_dir="$echo_dir/bin/win10"
echo_dir="$(cd "$echo_dir" && pwd)"
[ -f "$echo_dir/echovr.exe" ] || die "no echovr.exe in $echo_dir"
[ -f "$echo_dir/EchoXR.exe" ] && [ -f "$echo_dir/EchoXR/LibOVRRT64_1.dll" ] ||
    die "EchoXR isn't installed in $echo_dir -- unzip the EchoXR release there first"
game_root="$(cd "$echo_dir/../.." && pwd)"
# pnsovr.dll (the platform/login layer) imports LibOVRPlatform64_1.dll. On Windows it
# comes from the Oculus app's folder; a Linux prefix has none, so it must sit next to Echo.
if [ -f "$echo_dir/pnsovr.dll" ] && [ ! -f "$echo_dir/LibOVRPlatform64_1.dll" ]; then
    say "warning: no LibOVRPlatform64_1.dll in $echo_dir -- Echo can't log in without it."
    say "         Copy it (and LibOVRPlatformImpl64_1.dll) from your Windows PC's"
    say "         C:\\Program Files\\Oculus\\Support\\oculus-runtime\\ into bin/win10."
fi

# ---- Steam, and the libraries it knows about ------------------------------------
steam_root=""
for d in "$HOME/.steam/root" "$HOME/.local/share/Steam" "$HOME/.steam/steam" \
         "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" \
         "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"; do
    if [ -d "$d/steamapps" ]; then steam_root="$(cd "$d" && pwd -P)"; break; fi
done
[ -n "$steam_root" ] || die "Steam isn't installed (needed for Proton and SteamVR)"
libraries=("$steam_root")
if [ -f "$steam_root/steamapps/libraryfolders.vdf" ]; then
    while IFS= read -r p; do [ -d "$p/steamapps" ] && libraries+=("$p"); done < \
        <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$steam_root/steamapps/libraryfolders.vdf")
fi
find_common() {   # find_common <pattern> -> highest-versioned match in any library's steamapps/common
    local lib m
    for lib in "${libraries[@]}"; do
        for m in "$lib/steamapps/common/"*; do
            # shellcheck disable=SC2254  # $1 is a pattern on purpose
            case "$(basename "$m")" in $1) printf '%s\n' "$m" ;; esac
        done
    done | sort -V | tail -n1
}

# ---- Proton ---------------------------------------------------------------------
proton="${ECHOXR_PROTON:-}"
if [ -z "$proton" ]; then
    proton="$(ls -d "$steam_root/compatibilitytools.d/"GE-Proton* 2>/dev/null | sort -V | tail -n1 || true)"
    [ -n "$proton" ] || proton="$(find_common "Proton - Experimental" || true)"
    [ -n "$proton" ] || proton="$(find_common "Proton [0-9]*" || true)"
fi
[ -n "$proton" ] && [ -x "$proton/proton" ] ||
    die "no Proton found -- install Proton Experimental or GE-Proton in Steam, or set ECHOXR_PROTON"
wine_lib=""
for d in "$proton/files/lib/wine" "$proton/files/lib64/wine" "$proton/dist/lib64/wine"; do
    [ -d "$d" ] && wine_lib="$d" && break
done
if [ -z "$wine_lib" ] || ! ls "$wine_lib"/x86_64-windows/wineopenxr.dll "$wine_lib"/x86_64-unix/wineopenxr.so >/dev/null 2>&1; then
    die "$(basename "$proton") has no wineopenxr bridge -- use Proton 8 or newer, or GE-Proton"
fi

# ---- the OpenXR runtime ---------------------------------------------------------
runtime="${XR_RUNTIME_JSON:-}"
if [ -z "$runtime" ]; then
    for f in "${XDG_CONFIG_HOME:-$HOME/.config}/openxr/1/active_runtime.json" /etc/xdg/openxr/1/active_runtime.json; do
        [ -f "$f" ] && runtime="$f" && break
    done
fi
[ -n "$runtime" ] && [ -f "$runtime" ] ||
    die "no active OpenXR runtime -- set one in SteamVR/Monado/WiVRn, or export XR_RUNTIME_JSON=/path/to/manifest.json"
runtime="$(readlink -f "$runtime")"

# ---- the prefix and the environment Proton needs --------------------------------
data="${XDG_DATA_HOME:-$HOME/.local/share}/echoxr"
prefix="${ECHOXR_PREFIX:-$data/prefix}"
mkdir -p "$prefix"

export STEAM_COMPAT_DATA_PATH="$prefix"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam_root"
export STEAM_COMPAT_INSTALL_PATH="$game_root"
export STEAM_COMPAT_LIBRARY_PATHS="$(IFS=:; echo "${libraries[*]/%//steamapps}")"
export SteamAppId=0 SteamGameId=0
export XR_RUNTIME_JSON="$runtime"
export PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1          # let the container see the runtime
export PRESSURE_VESSEL_FILESYSTEMS_RW="$game_root:$prefix${PRESSURE_VESSEL_FILESYSTEMS_RW:+:$PRESSURE_VESSEL_FILESYSTEMS_RW}"
# dbgcore.dll in bin/win10 is the plugin loader; Wine would use its own builtin one
export WINEDLLOVERRIDES="dbgcore=n,b${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
if [ "${ECHOXR_DEBUG:-0}" != 0 ]; then
    mkdir -p "$data/logs"
    export PROTON_LOG=1 PROTON_LOG_DIR="$data/logs" XR_LOADER_DEBUG=all
    say "debug logs in $data/logs"
fi

say "Echo:    $echo_dir"
say "Proton:  $proton"
say "OpenXR:  $runtime"
say "prefix:  $prefix"

cmd=("$proton/proton" waitforexitandrun "$echo_dir/EchoXR.exe" "${args[@]}")
slr="$(find_common "SteamLinuxRuntime_sniper" || true)"
if [ "${ECHOXR_NO_CONTAINER:-0}" = 0 ] && [ -n "$slr" ] && [ -x "$slr/_v2-entry-point" ]; then
    say "container: $slr"
    cd "$echo_dir"
    exec "$slr/_v2-entry-point" --verb=waitforexitandrun -- "${cmd[@]}"
fi
say "container: none (running Proton directly)"
cd "$echo_dir"
exec "${cmd[@]}"
