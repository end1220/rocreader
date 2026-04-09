#!/bin/sh
set -eu

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$SELF_DIR/ROCreader"
BIN="$APP_DIR/rocreader_sdl"
LOG_FILE="${ROC_NATIVE_RUNTIME_LOG:-$SELF_DIR/ROCreader.log}"
LIB_FULL_DIR="$APP_DIR/lib"
LIB_SYSTEM_SDL_DIR="$APP_DIR/lib_system_sdl"
LIB_DIR="$LIB_FULL_DIR"
UPDATE_STATUS_FILE="$APP_DIR/cache/update_boot_status.txt"
UPDATE_STAGE_DIR="$APP_DIR/cache/update_stage"

export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
export SDL_NOMOUSE="${SDL_NOMOUSE:-1}"
export ROCREADER_ROOT="$APP_DIR"
export ROCREADER_CARD1_ROOT="/mnt/mmc"
export ROCREADER_CARD2_ROOT="/mnt/sdcard"
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
  export XDG_RUNTIME_DIR="/tmp/rocreader-xdg"
fi
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null || true
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true
cd "$APP_DIR"

set_runtime_libs() {
  lib_dir="$1"
  if [ -d "$lib_dir" ]; then
    LIB_DIR="$lib_dir"
  else
    LIB_DIR="$LIB_FULL_DIR"
  fi
  export LD_LIBRARY_PATH="$LIB_DIR:$LIB_DIR/pulseaudio:/usr/lib32:/usr/lib:/lib:/mnt/vendor/lib:${LD_LIBRARY_PATH_BASE:-}"
}

log_line() {
  printf '%s\n' "$1" >>"$LOG_FILE"
}

write_update_status() {
  result="$1"
  version="${2:-}"
  mkdir -p "$(dirname "$UPDATE_STATUS_FILE")" 2>/dev/null || true
  {
    printf 'result=%s\n' "$result"
    [ -n "$version" ] && printf 'version=%s\n' "$version"
  } >"$UPDATE_STATUS_FILE"
}

find_pending_marker() {
  for root in /mnt/mmc /mnt/sdcard; do
    marker="$root/Downloads/ROCreader_update_pending.txt"
    [ -f "$marker" ] && { printf '%s' "$marker"; return 0; }
  done
  return 1
}

extract_marker_value() {
  key="$1"
  marker="$2"
  awk -F= -v wanted="$key" '$1 == wanted { print substr($0, index($0, "=") + 1); exit }' "$marker"
}

extract_zip_to_stage() {
  zip_file="$1"
  stage_dir="$2"
  rm -rf "$stage_dir"
  mkdir -p "$stage_dir"
  if command -v unzip >/dev/null 2>&1; then
    unzip -oq "$zip_file" -d "$stage_dir" >>"$LOG_FILE" 2>&1
    return $?
  fi
  if command -v busybox >/dev/null 2>&1; then
    busybox unzip -o "$zip_file" -d "$stage_dir" >>"$LOG_FILE" 2>&1
    return $?
  fi
  return 127
}

replace_runtime_entry() {
  name="$1"
  src="$2/$name"
  dst="$APP_DIR/$name"
  [ -e "$src" ] || return 0
  rm -rf "$dst"
  cp -a "$src" "$APP_DIR/"
}

perform_pending_update_if_any() {
  marker="$(find_pending_marker || true)"
  [ -n "$marker" ] || return 0

  package_dir="$(dirname "$marker")"
  package_name="$(extract_marker_value filename "$marker")"
  package_version="$(extract_marker_value version "$marker")"
  package_path="$package_dir/$package_name"
  staged_runtime="$UPDATE_STAGE_DIR/Roms/APPS/ROCreader"
  staged_launcher="$UPDATE_STAGE_DIR/Roms/APPS/ROCreader.sh"

  log_line "[update] pending marker: $marker"
  log_line "[update] package: $package_path"

  if [ -z "$package_name" ] || [ ! -f "$package_path" ]; then
    log_line "[update] missing package, skip install"
    write_update_status "failed" "$package_version"
    return 0
  fi

  if ! extract_zip_to_stage "$package_path" "$UPDATE_STAGE_DIR"; then
    log_line "[update] extract failed"
    write_update_status "failed" "$package_version"
    return 0
  fi

  if [ ! -d "$staged_runtime" ]; then
    log_line "[update] staged runtime missing: $staged_runtime"
    write_update_status "failed" "$package_version"
    rm -rf "$UPDATE_STAGE_DIR"
    return 0
  fi

  replace_runtime_entry "rocreader_sdl" "$staged_runtime"
  replace_runtime_entry "ui.pack" "$staged_runtime"
  replace_runtime_entry "native_config.ini" "$staged_runtime"
  replace_runtime_entry "native_keymap.ini" "$staged_runtime"
  replace_runtime_entry "fonts" "$staged_runtime"
  replace_runtime_entry "sounds" "$staged_runtime"
  replace_runtime_entry "lib" "$staged_runtime"
  replace_runtime_entry "lib_system_sdl" "$staged_runtime"

  if [ -f "$staged_launcher" ]; then
    cp "$staged_launcher" "$SELF_DIR/ROCreader.sh.new"
    mv "$SELF_DIR/ROCreader.sh.new" "$SELF_DIR/ROCreader.sh"
  fi

  chmod +x "$APP_DIR/rocreader_sdl" 2>/dev/null || true
  chmod +x "$SELF_DIR/ROCreader.sh" 2>/dev/null || true

  rm -f "$marker"
  rm -rf "$UPDATE_STAGE_DIR"
  write_update_status "success" "$package_version"
  log_line "[update] install success version=${package_version:-unknown}"
}

run_with_driver() {
  drv="$1"
  mode="$2"
  lib_dir="$3"
  set_runtime_libs "$lib_dir"
  SDL_VIDEODRIVER="$drv" "$BIN" >>"$LOG_FILE" 2>&1
}

run_default() {
  mode="$1"
  lib_dir="$2"
  set_runtime_libs "$lib_dir"
  "$BIN" >>"$LOG_FILE" 2>&1
}

LD_LIBRARY_PATH_BASE="${LD_LIBRARY_PATH:-}"
set_runtime_libs "$LIB_SYSTEM_SDL_DIR"

if [ ! -x "$BIN" ]; then
  log_line "[launcher] binary missing: $BIN"
  exit 4
fi

log_line "===== $(date '+%F %T %Z') ====="
perform_pending_update_if_any

if [ -n "${SDL_VIDEODRIVER:-}" ]; then
  run_with_driver "$SDL_VIDEODRIVER" "forced" "$LIB_FULL_DIR"
  exit $?
fi

try_mode() {
  mode="$1"
  lib_dir="$2"

  if run_default "$mode" "$lib_dir"; then
    exit 0
  fi

  for drv in KMSDRM kmsdrm wayland x11; do
    if run_with_driver "$drv" "$mode" "$lib_dir"; then
      exit 0
    fi
  done
}

try_mode "system_sdl" "$LIB_SYSTEM_SDL_DIR"
try_mode "full" "$LIB_FULL_DIR"
log_line "[launcher] all drivers failed"
exit 5
