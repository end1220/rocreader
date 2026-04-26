param(
    [string]$ImageName = "trimui-smart-pro-toolchain",
    [string]$BaseImage = "debian:bullseye-slim",
    [ValidateSet("0", "1")]
    [string]$RequireMupdf = "1",
    [switch]$SkipCMake,
    [switch]$NoBuildImage
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$ToolchainDir = Join-Path $ProjectRoot "trimui-smart-pro-toolchain"
$WorkspaceDir = Join-Path $ScriptDir "workspace"
$SourceDir = Join-Path $WorkspaceDir "source"
$DistDir = Join-Path $ScriptDir "dist_lowglibc"
$DownloadsDir = Join-Path $ScriptDir "Downloads"
$LogsDir = Join-Path $ScriptDir "logs"
$SysrootOverlayDir = Join-Path $ScriptDir "sysroot_overlay"

if (-not (Test-Path (Join-Path $ToolchainDir "Dockerfile"))) {
    throw "Toolchain Dockerfile not found: $ToolchainDir"
}

docker --version | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "Docker is not available."
}

if (-not $NoBuildImage) {
    $installCMake = if ($SkipCMake) { "0" } else { "1" }
    docker build `
        --build-arg "BASE_IMAGE=$BaseImage" `
        --build-arg "INSTALL_CMAKE=$installCMake" `
        -t $ImageName `
        $ToolchainDir
    if ($LASTEXITCODE -ne 0) {
        throw "Docker image build failed for $ImageName."
    }
}

New-Item -ItemType Directory -Force -Path $WorkspaceDir, $DistDir, $DownloadsDir, $LogsDir | Out-Null

$projectMount = ($ProjectRoot -replace "\\", "/")

$containerScript = @'
set -euo pipefail

PROJECT=/root/workspace/ROCreader
SRC="$PROJECT/TrimuiBrick/workspace/source"

mkdir -p "$SRC"
rsync -a --delete \
  --exclude='.git/' \
  --exclude='.vs/' \
  --exclude='TrimuiBrick/' \
  --exclude='trimui-smart-pro-toolchain/' \
  --exclude='sysroot_device/' \
  --exclude='build/' \
  --exclude='dist_lowglibc/' \
  --exclude='dist_h700/' \
  --exclude='Downloads/' \
  --exclude='logs/' \
  --exclude='cache/' \
  --exclude='book_covers/' \
  --exclude='books/' \
  --exclude='src/*.o' \
  "$PROJECT/" "$SRC/"

cd "$SRC"
export PATH="/opt/aarch64-linux-gnu-7.5.0-linaro/bin:$PATH"
export SYSROOT="/opt/aarch64-linux-gnu-7.5.0-linaro/sysroot"
build_brick_static_webp() {
  dst_prefix="$PROJECT/TrimuiBrick/workspace/static_webp"
  if [ -f "$dst_prefix/lib/libwebp.a" ] && [ -f "$dst_prefix/include/webp/decode.h" ]; then
    return 0
  fi
  build_dir="$PROJECT/TrimuiBrick/workspace/webp-build"
  rm -rf "$build_dir" "$dst_prefix"
  mkdir -p "$build_dir"
  cd "$build_dir"
  if [ -f "$PROJECT/TrimuiBrick/deps/libwebp-1.3.2.tar.gz" ]; then
    cp "$PROJECT/TrimuiBrick/deps/libwebp-1.3.2.tar.gz" .
  else
    wget -q https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.3.2.tar.gz
  fi
  tar xf libwebp-1.3.2.tar.gz
  cd libwebp-1.3.2
  ./configure \
    --host=aarch64-linux-gnu \
    --prefix="$dst_prefix" \
    --disable-shared \
    --enable-static \
    --disable-libwebpmux \
    --disable-libwebpdemux \
    --disable-libwebpdecoder \
    --disable-libwebpextras \
    --disable-sdl \
    CC=aarch64-linux-gnu-gcc \
    CXX=aarch64-linux-gnu-g++ \
    AR=aarch64-linux-gnu-ar \
    RANLIB=aarch64-linux-gnu-ranlib
  make -j"$(nproc)"
  make install
  cd "$SRC"
}
build_brick_static_webp
if [ -d "$PROJECT/TrimuiBrick/sysroot_overlay" ]; then
  rsync -a "$PROJECT/TrimuiBrick/sysroot_overlay/" "$SYSROOT/"
fi
export CROSS_TOOL_PREFIX="aarch64-linux-gnu"
export CROSS_CXX="aarch64-linux-gnu-g++"
export CROSS_READELF="aarch64-linux-gnu-readelf"
export CROSS_PKG_CONFIG="pkg-config"
export ROC_NATIVE_LOG_DIR="$PROJECT/TrimuiBrick/logs"
export DIST_ROOT="$PROJECT/TrimuiBrick/dist_lowglibc"
export DOWNLOADS_ROOT="$PROJECT/TrimuiBrick/Downloads"
export TRIMUI_BRICK_LAYOUT=1
export REQUIRE_MUPDF="${REQUIRE_MUPDF:-1}"
export WEBP_CFLAGS="-I$PROJECT/TrimuiBrick/workspace/static_webp/include"
export WEBP_LIBS="$PROJECT/TrimuiBrick/workspace/static_webp/lib/libwebp.a -lm"

./cross_compile_low_glibc.sh
'@

$ContainerScriptPath = Join-Path $WorkspaceDir "build_low_glibc_container.sh"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($ContainerScriptPath, $containerScript, $Utf8NoBom)

docker run --rm `
    -e "REQUIRE_MUPDF=$RequireMupdf" `
    -v "${projectMount}:/root/workspace/ROCreader" `
    -w /root/workspace/ROCreader `
    $ImageName `
    bash /root/workspace/ROCreader/TrimuiBrick/workspace/build_low_glibc_container.sh
if ($LASTEXITCODE -ne 0) {
    throw "Trimui Brick Docker package build failed."
}

Write-Host ""
Write-Host "Trimui Brick package outputs:"
Write-Host "  Dist:      $DistDir"
Write-Host "  Downloads: $DownloadsDir"
Write-Host "  Logs:      $LogsDir"
