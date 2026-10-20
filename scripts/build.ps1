param(
    [ValidateSet('debug', 'asan', 'release')]
    [string]$Preset = 'debug',
    [string]$TestFilter = '',
    [switch]$Quick
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'msvc_environment.ps1') -Version '[17.0,18.0)'

Push-Location -LiteralPath $projectRoot
try {
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "Configure failed: $LASTEXITCODE" }
    & cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
    if ($Preset -ne 'release') {
        if ($TestFilter -or $Quick) {
            $testArguments = @()
            if ($TestFilter) { $testArguments += "--gtest_filter=$TestFilter" }
            if ($Quick) { $testArguments += '--quick' }
            & (Join-Path $projectRoot "build/$Preset/lsm_tests.exe") @testArguments
        } else {
            & ctest --preset $Preset --verbose
        }
        if ($LASTEXITCODE -ne 0) { throw "Tests failed: $LASTEXITCODE" }
    }
} finally {
    Pop-Location
}
