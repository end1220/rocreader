#!/bin/sh
set -eu

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$SELF_DIR/ROCreader"
BIN="$APP_DIR/rocreader_sdl"
LOG_FILE="${ROC_NATIVE_RUNTIME_LOG:-$SELF_DIR/ROCreader/ROCreader.log}"
LIB_FULL_DIR="$APP_DIR/lib"
LIB_SYSTEM_SDL_DIR="$APP_DIR/lib_system_sdl"
LIB_DIR="$LIB_FULL_DIR"

export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
export SDL_NOMOUSE="${SDL_NOMOUSE:-1}"
export ROCREADER_ROOT="$APP_DIR"
export ROCREADER_CACHE_ROOT="${ROCREADER_CACHE_ROOT:-$APP_DIR/cache}"
export ROCREADER_CARD1_ROOT="/mnt/mmc"
export ROCREADER_CARD2_ROOT="/mnt/sdcard"
export ROCREADER_SCAN_CARD2="${ROCREADER_SCAN_CARD2:-1}"
export ROCREADER_FULL_INPUT_LOG="${ROCREADER_FULL_INPUT_LOG:-0}"
export ROCREADER_LOG_MAX_BYTES="${ROCREADER_LOG_MAX_BYTES:-524288}"
# Disable update endpoint for MiniLoong launcher.
export ROCREADER_UPDATE_CONTENTS_URL=""
export ROCREADER_DISABLE_UPDATE="1"

# MiniLoong fixed profile: only target 960x720.
export ROCREADER_SCREEN_PROFILE="960x720"
export ROCREADER_SCREEN_W="960"
export ROCREADER_SCREEN_H="720"
export ROCREADER_DEVICE_MODEL="miniloong"

if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
  export XDG_RUNTIME_DIR="/tmp/rocreader-xdg"
fi
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null || true
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true
cd "$APP_DIR"

# Start each run with a fresh log file.
mkdir -p "$(dirname "$LOG_FILE")" 2>/dev/null || true
: >"$LOG_FILE"

set_runtime_libs() {
  lib_dir="$1"
  if [ -d "$lib_dir" ]; then
    LIB_DIR="$lib_dir"
  else
    LIB_DIR="$LIB_FULL_DIR"
  fi
  export LD_LIBRARY_PATH="$LIB_DIR:$LIB_DIR/pulseaudio:/usr/lib32:/usr/lib:/lib:/mnt/vendor/lib:${LD_LIBRARY_PATH_BASE:-}"
  # ALSA plugins are resolved from ALSA_PLUGIN_DIR, not LD_LIBRARY_PATH.
  if [ -d "$LIB_DIR/alsa-lib" ]; then
    export ALSA_PLUGIN_DIR="$LIB_DIR/alsa-lib"
  elif [ -d "$LIB_DIR/alsa/alsa-lib" ]; then
    export ALSA_PLUGIN_DIR="$LIB_DIR/alsa/alsa-lib"
  else
    unset ALSA_PLUGIN_DIR 2>/dev/null || true
  fi
}

log_line() {
  if [ "${ROCREADER_FULL_INPUT_LOG:-0}" = "1" ]; then
    printf '%s\n' "$1" >>"$LOG_FILE"
    return 0
  fi
  if [ "${ROCREADER_VERBOSE_LOG:-0}" != "1" ] && [ "${ROCREADER_DEBUG_LOG:-0}" != "1" ]; then
    case "$1" in
      *failed*|*Failed*|*FAILED*|*error*|*Error*|*ERROR*|*missing*|*Missing*|*MISSING*|*crash*|*Crash*|*CRASH*|*fatal*|*Fatal*|*FATAL*) ;;
      *) return 0 ;;
    esac
  fi
  printf '%s\n' "$1" >>"$LOG_FILE"
}

trim_log_if_needed() {
  [ "${ROCREADER_LOG_MAX_BYTES:-0}" -gt 0 ] 2>/dev/null || return 0
  [ -f "$LOG_FILE" ] || return 0
  size="$(wc -c <"$LOG_FILE" 2>/dev/null || printf '0')"
  [ "$size" -le "$ROCREADER_LOG_MAX_BYTES" ] 2>/dev/null || : >"$LOG_FILE"
}

run_with_driver() {
  drv="$1"
  lib_dir="$2"
  set_runtime_libs "$lib_dir"
  SDL_VIDEODRIVER="$drv" "$BIN" >>"$LOG_FILE" 2>&1
}

run_default() {
  lib_dir="$1"
  set_runtime_libs "$lib_dir"
  "$BIN" >>"$LOG_FILE" 2>&1
}

LD_LIBRARY_PATH_BASE="${LD_LIBRARY_PATH:-}"
set_runtime_libs "$LIB_SYSTEM_SDL_DIR"

if [ ! -x "$BIN" ]; then
  log_line "[launcher:miniloong] binary missing: $BIN"
  exit 4
fi

trim_log_if_needed
log_line "===== $(date '+%F %T %Z') ====="
log_line "[launcher:miniloong] fixed screen profile: ${ROCREADER_SCREEN_W}x${ROCREADER_SCREEN_H}"

if [ -n "${SDL_VIDEODRIVER:-}" ]; then
  run_with_driver "$SDL_VIDEODRIVER" "$LIB_FULL_DIR"
  exit $?
fi

try_mode() {
  lib_dir="$1"
  if run_default "$lib_dir"; then
    exit 0
  fi
  for drv in KMSDRM kmsdrm wayland x11; do
    if run_with_driver "$drv" "$lib_dir"; then
      exit 0
    fi
  done
}

try_mode "$LIB_SYSTEM_SDL_DIR"
try_mode "$LIB_FULL_DIR"
log_line "[launcher:miniloong] all drivers failed"
exit 5
