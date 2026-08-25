$ErrorActionPreference = "Stop"

Write-Host "Checking for CMake..."
if (!(Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error "CMake is not installed or not in PATH."
    exit 1
}

$BuildDir = "build"
if (!(Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

$CmakeArgs = @("-S", ".", "-B", $BuildDir)

# Use Ninja if available
if (Get-Command ninja -ErrorAction SilentlyContinue) {
    Write-Host "Ninja found. Using Ninja generator."
    $CmakeArgs += "-G", "Ninja"
} else {
    Write-Host "Ninja not found. Falling back to default generator."
}

# Integrate with vcpkg if VCPKG_ROOT is set
if ($env:VCPKG_ROOT) {
    Write-Host "VCPKG_ROOT environment variable found. Integrating vcpkg."
    $CmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
}

Write-Host "Configuring project..."
& cmake @CmakeArgs

Write-Host "Building project..."
& cmake --build $BuildDir --config Release

Write-Host "Build completed successfully."
