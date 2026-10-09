#Requires -Version 5.1
# Windows verification: lint → configure → build → unit → integration → ui → smoke.
param([string]$BuildType = "Release")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build"
if (Get-Command bash -ErrorAction SilentlyContinue) {
    bash (Join-Path $PSScriptRoot "lint.sh")
} else {
    Write-Warning "bash not found; skipping scripts/lint.sh (CI runs it)"
}
$Prefix = if ($env:CMAKE_PREFIX_PATH) { $env:CMAKE_PREFIX_PATH } else { "C:/tools/msys64/ucrt64" }
cmake -S $Root -B $Build -DCMAKE_PREFIX_PATH="$Prefix" -DCMAKE_BUILD_TYPE=$BuildType
cmake --build $Build --config $BuildType
ctest --test-dir $Build -L unit --output-on-failure
ctest --test-dir $Build -L integration --output-on-failure
ctest --test-dir $Build -L ui --output-on-failure
& (Join-Path $Build "src/UniversalStorageAnalyzer.exe") --smoke-test
& (Join-Path $Build "src/UniversalStorageAnalyzer.exe") --headless-check
Write-Host "VERIFY-OK"
