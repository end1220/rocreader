param(
    [string]$ImageName = "rocreader-h700-low-glibc",
    [string]$BaseImage = "ubuntu:22.04",
    [string]$AptMirror = "http://mirrors.aliyun.com/ubuntu",
    [ValidateSet("0", "1")]
    [string]$RequireMupdf = "1",
    [switch]$NoBuildImage
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolchainDir = Join-Path $ProjectRoot "h700-low-glibc-toolchain"
$LogsDir = Join-Path $ProjectRoot "logs"

if (-not (Test-Path (Join-Path $ToolchainDir "Dockerfile"))) {
    throw "H700 Dockerfile not found: $ToolchainDir"
}

New-Item -ItemType Directory -Force -Path $LogsDir | Out-Null

docker version | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "Docker is not available. Start Docker Desktop before building H700 packages."
}

if (-not $NoBuildImage) {
    docker build `
        --build-arg "BASE_IMAGE=$BaseImage" `
        --build-arg "APT_MIRROR=$AptMirror" `
        -t $ImageName `
        $ToolchainDir
    if ($LASTEXITCODE -ne 0) {
        throw "Docker image build failed for $ImageName."
    }
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$dockerLog = Join-Path $LogsDir "docker_h700_low_glibc_$timestamp.log"
$projectMount = ($ProjectRoot -replace "\\", "/")

docker run --rm `
    -e "TRIMUI_BRICK_LAYOUT=0" `
    -e "REQUIRE_MUPDF=$RequireMupdf" `
    -v "${projectMount}:/work" `
    -w /work `
    $ImageName `
    bash ./cross_compile_low_glibc.sh 2>&1 | Tee-Object -FilePath $dockerLog
if ($LASTEXITCODE -ne 0) {
    throw "H700 Docker package build failed."
}

Write-Host ""
Write-Host "H700 package outputs:"
Write-Host "  Downloads: $(Join-Path $ProjectRoot 'Downloads')"
Write-Host "  Dist:      $(Join-Path $ProjectRoot 'dist_lowglibc')"
Write-Host "  Logs:      $LogsDir"
Write-Host "  DockerLog: $dockerLog"
