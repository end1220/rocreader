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
INSTALLED_VERSION_FILE="$APP_DIR/version.txt"

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

read_installed_version() {
  [ -f "$INSTALLED_VERSION_FILE" ] || return 0
  sed -n '1p' "$INSTALLED_VERSION_FILE"
}

write_installed_version() {
  version="$1"
  [ -n "$version" ] || return 0
  printf '%s\n' "$version" >"$INSTALLED_VERSION_FILE"
}

read_config_screen_profile() {
  config_path="$APP_DIR/native_config.ini"
  [ -r "$config_path" ] || return 1
  profile="$(awk -F= '$1 == "screen_profile" { print $2; exit }' "$config_path" | tr -d '\r' | tr 'A-Z' 'a-z')"
  case "$profile" in
    640x480|640|720x480|720)
      printf '%s\n' "$profile"
      return 0
      ;;
  esac
  return 1
}

read_device_model_name() {
  if [ -n "${ROCREADER_DEVICE_MODEL:-}" ]; then
    printf '%s\n' "$ROCREADER_DEVICE_MODEL" | tr -d '\r' | tr 'A-Z' 'a-z'
    return 0
  fi
  for path in \
    /sys/firmware/devicetree/base/model \
    /proc/device-tree/model \
    /tmp/device_info.txt \
    /tmp/sysinfo/model \
    /proc/cmdline \
    /etc/hostname \
    /sys/devices/virtual/dmi/id/product_name \
    /sys/devices/virtual/dmi/id/board_name; do
    [ -r "$path" ] || continue
    tr -d '\000\r' <"$path" | tr 'A-Z' 'a-z' | head -n 1
    return 0
  done
  return 1
}

log_device_model_candidates() {
  if [ -n "${ROCREADER_DEVICE_MODEL:-}" ]; then
    log_line "[launcher] model candidate env: $(printf '%s' "$ROCREADER_DEVICE_MODEL" | tr -d '\r' | tr 'A-Z' 'a-z')"
  else
    log_line "[launcher] model candidate env: none"
  fi
  for path in \
    /sys/firmware/devicetree/base/model \
    /proc/device-tree/model \
    /tmp/device_info.txt \
    /tmp/sysinfo/model \
    /proc/cmdline \
    /etc/hostname \
    /sys/devices/virtual/dmi/id/product_name \
    /sys/devices/virtual/dmi/id/board_name; do
    if [ -r "$path" ]; then
      value="$(tr -d '\000\r' <"$path" | tr 'A-Z' 'a-z' | head -n 1)"
      if [ -n "$value" ]; then
        log_line "[launcher] model candidate $path: $value"
      else
        log_line "[launcher] model candidate $path: <empty>"
      fi
    else
      log_line "[launcher] model candidate $path: <unreadable>"
    fi
  done
}

log_screen_size_candidates() {
  for path in \
    /sys/class/graphics/fb0/modes \
    /sys/class/graphics/fb0/mode \
    /sys/class/graphics/fb1/modes \
    /sys/class/graphics/fb1/mode \
    /sys/class/graphics/fb0/virtual_size \
    /sys/class/graphics/fb1/virtual_size; do
    if [ -r "$path" ]; then
      value="$(tr -d '\000\r' <"$path" | head -n 1)"
      if [ -n "$value" ]; then
        log_line "[launcher] screen candidate $path: $value"
      else
        log_line "[launcher] screen candidate $path: <empty>"
      fi
    else
      log_line "[launcher] screen candidate $path: <unreadable>"
    fi
  done
}

screen_profile_from_device_model() {
  model_name="${ROCREADER_DEVICE_MODEL:-}"
  case "$model_name" in
    *rg34xx*|*34xx*)
      printf '720x480|%s\n' "$model_name"
      return 0
      ;;
    *rg35xx*|*35xx*|*rg40xx*|*40xx*)
      printf '640x480|%s\n' "$model_name"
      return 0
      ;;
  esac
  model_token="$(printf '%s\n' "$model_name" | sed -n 's/.*\([a-z0-9_-]*34xx[a-z0-9_-]*\).*/\1/p' | head -n 1)"
  case "$model_token" in
    *34xx*)
      printf '720x480|%s\n' "$model_token"
      return 0
      ;;
  esac
  model_token="$(printf '%s\n' "$model_name" | sed -n 's/.*\([a-z0-9_-]*35xx[a-z0-9_-]*\).*/\1/p' | head -n 1)"
  case "$model_token" in
    *35xx*)
      printf '640x480|%s\n' "$model_token"
      return 0
      ;;
  esac
  model_token="$(printf '%s\n' "$model_name" | sed -n 's/.*\([a-z0-9_-]*40xx[a-z0-9_-]*\).*/\1/p' | head -n 1)"
  case "$model_token" in
    *40xx*)
      printf '640x480|%s\n' "$model_token"
      return 0
      ;;
  esac

  for path in \
    /sys/firmware/devicetree/base/model \
    /proc/device-tree/model \
    /tmp/device_info.txt \
    /tmp/sysinfo/model \
    /proc/cmdline \
    /etc/hostname \
    /sys/devices/virtual/dmi/id/product_name \
    /sys/devices/virtual/dmi/id/board_name; do
    [ -r "$path" ] || continue
    model_name="$(tr -d '\000\r' <"$path" | tr 'A-Z' 'a-z' | head -n 1)"
    case "$model_name" in
      *rg34xx*|*34xx*)
        printf '720x480|%s\n' "$model_name"
        return 0
        ;;
      *rg35xx*|*35xx*|*rg40xx*|*40xx*)
        printf '640x480|%s\n' "$model_name"
        return 0
        ;;
    esac
    model_token="$(printf '%s\n' "$model_name" | sed -n 's/.*\([a-z0-9_-]*34xx[a-z0-9_-]*\).*/\1/p' | head -n 1)"
    case "$model_token" in
      *34xx*)
        printf '720x480|%s\n' "$model_token"
        return 0
        ;;
    esac
    model_token="$(printf '%s\n' "$model_name" | sed -n 's/.*\([a-z0-9_-]*35xx[a-z0-9_-]*\).*/\1/p' | head -n 1)"
    case "$model_token" in
      *35xx*)
        printf '640x480|%s\n' "$model_token"
        return 0
        ;;
    esac
    model_token="$(printf '%s\n' "$model_name" | sed -n 's/.*\([a-z0-9_-]*40xx[a-z0-9_-]*\).*/\1/p' | head -n 1)"
    case "$model_token" in
      *40xx*)
        printf '640x480|%s\n' "$model_token"
        return 0
        ;;
    esac
  done
  return 1
}

normalize_screen_override() {
  case "${ROCREADER_SCREEN_PROFILE:-}" in
    640x480|640)
      export ROCREADER_SCREEN_W=640
      export ROCREADER_SCREEN_H=480
      ;;
    720x480|720)
      export ROCREADER_SCREEN_W=720
      export ROCREADER_SCREEN_H=480
      ;;
  esac
}

detect_screen_size_token() {
  for path in \
    /sys/class/graphics/fb0/modes \
    /sys/class/graphics/fb0/mode \
    /sys/class/graphics/fb1/modes \
    /sys/class/graphics/fb1/mode \
    /sys/class/graphics/fb0/virtual_size \
    /sys/class/graphics/fb1/virtual_size; do
    [ -r "$path" ] || continue
    value="$(tr -d '\000\r' <"$path" | head -n 1)"
    case "$value" in
      *640*)
        printf '%s\n' "640x480"
        return 0
        ;;
      *720*)
        printf '%s\n' "720x480"
        return 0
        ;;
    esac
  done
  return 1
}

ensure_screen_override_if_needed() {
  normalize_screen_override
  if [ -n "${ROCREADER_SCREEN_W:-}" ] && [ -n "${ROCREADER_SCREEN_H:-}" ]; then
    log_line "[launcher] screen override preset: ${ROCREADER_SCREEN_W}x${ROCREADER_SCREEN_H}"
    return 0
  fi

  config_profile="$(read_config_screen_profile || true)"
  case "$config_profile" in
    640x480|640)
      export ROCREADER_SCREEN_PROFILE=640x480
      export ROCREADER_SCREEN_W=640
      export ROCREADER_SCREEN_H=480
      log_line "[launcher] screen override config: 640x480"
      return 0
      ;;
    720x480|720)
      export ROCREADER_SCREEN_PROFILE=720x480
      export ROCREADER_SCREEN_W=720
      export ROCREADER_SCREEN_H=480
      log_line "[launcher] screen override config: 720x480"
      return 0
      ;;
  esac

  model_rule="$(screen_profile_from_device_model || true)"
  case "$model_rule" in
    640x480\|*)
      model_name="${model_rule#*|}"
      export ROCREADER_SCREEN_PROFILE=640x480
      export ROCREADER_SCREEN_W=640
      export ROCREADER_SCREEN_H=480
      log_line "[launcher] screen override model: $model_name -> 640x480"
      return 0
      ;;
    720x480\|*)
      model_name="${model_rule#*|}"
      export ROCREADER_SCREEN_PROFILE=720x480
      export ROCREADER_SCREEN_W=720
      export ROCREADER_SCREEN_H=480
      log_line "[launcher] screen override model: $model_name -> 720x480"
      return 0
      ;;
  esac
  model_name="$(read_device_model_name || true)"
  if [ -n "$model_name" ]; then
    log_line "[launcher] device model observed: $model_name"
  else
    log_line "[launcher] device model observed: none"
  fi
  log_device_model_candidates
  log_screen_size_candidates

  size="$(detect_screen_size_token || true)"
  case "$size" in
    640x480)
      export ROCREADER_SCREEN_W=640
      export ROCREADER_SCREEN_H=480
      log_line "[launcher] screen auto-detect override: 640x480"
      ;;
    720x480)
      export ROCREADER_SCREEN_W=720
      export ROCREADER_SCREEN_H=480
      log_line "[launcher] screen auto-detect override: 720x480"
      ;;
    *)
      log_line "[launcher] screen auto-detect override: none"
      ;;
  esac
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

extract_version_from_name() {
  file_name="$(basename "$1")"
  printf '%s\n' "$file_name" | sed -n 's/.*\(ver[0-9][0-9.]*\)\.zip$/\1/p'
}

version_sort_key() {
  version="$1"
  printf '%s\n' "$version" | awk '
    BEGIN { count = 0 }
    {
      n = split($0, parts, /[^0-9]+/)
      for (i = 1; i <= n; ++i) {
        if (parts[i] != "") {
          printf "%06d", parts[i]
          count++
        }
      }
    }
    END {
      if (count == 0) printf "000000"
    }'
}

version_is_newer() {
  candidate="$1"
  baseline="$2"
  [ "$(version_sort_key "$candidate")" \> "$(version_sort_key "$baseline")" ]
}

find_latest_download_zip() {
  best_path=""
  best_version=""
  for root in /mnt/mmc /mnt/sdcard; do
    downloads_dir="$root/Downloads"
    [ -d "$downloads_dir" ] || continue
    for zip_file in "$downloads_dir"/*.zip; do
      [ -f "$zip_file" ] || continue
      zip_version="$(extract_version_from_name "$zip_file")"
      [ -n "$zip_version" ] || continue
      if [ -z "$best_path" ] || version_is_newer "$zip_version" "$best_version"; then
        best_path="$zip_file"
        best_version="$zip_version"
      fi
    done
  done
  [ -n "$best_path" ] && printf '%s\n' "$best_path"
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
  installed_version="$(read_installed_version || true)"
  package_path=""
  package_version=""
  latest_zip="$(find_latest_download_zip || true)"
  latest_version=""
  if [ -n "$latest_zip" ]; then
    latest_version="$(extract_version_from_name "$latest_zip")"
  fi
  if [ -n "$latest_zip" ] && [ -n "$latest_version" ]; then
    if [ -z "$installed_version" ] || version_is_newer "$latest_version" "$installed_version"; then
      package_path="$latest_zip"
      package_version="$latest_version"
    fi
  fi
  if [ -z "$package_path" ] && [ -n "$marker" ]; then
    package_dir="$(dirname "$marker")"
    package_name="$(extract_marker_value filename "$marker")"
    package_version="$(extract_marker_value version "$marker")"
    package_path="$package_dir/$package_name"
    if [ -z "$package_version" ]; then
      package_version="$(extract_version_from_name "$package_path")"
    fi
  fi
  [ -n "$package_path" ] || return 0

  staged_runtime="$UPDATE_STAGE_DIR/Roms/APPS/ROCreader"
  staged_launcher="$UPDATE_STAGE_DIR/Roms/APPS/ROCreader.sh"

  log_line "[update] pending marker: $marker"
  log_line "[update] installed version: ${installed_version:-unknown}"
  log_line "[update] latest zip: ${latest_zip:-none}"
  log_line "[update] package: $package_path"
  log_line "[update] package version: ${package_version:-unknown}"

  if [ ! -f "$package_path" ]; then
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

  write_installed_version "$package_version"
  rm -f /mnt/mmc/Downloads/ROCreader_update_pending.txt /mnt/sdcard/Downloads/ROCreader_update_pending.txt
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
ensure_screen_override_if_needed

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
