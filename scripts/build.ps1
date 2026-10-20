param(
    [ValidateSet('debug', 'asan', 'release')]
    [string]$Preset = 'debug',
    [string]$TestFilter = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswherePath)) {
    throw 'Install Visual Studio 2022 Build Tools with the Desktop development with C++ workload.'
}
$vsInstall = & $vswherePath -latest -products '*' -version '[17.0,18.0)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) {
    throw 'No Visual Studio 2022 C++ toolchain was found.'
}
$developerCommand = Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat'
$environmentLines = & $env:ComSpec /d /s /c "`"$developerCommand`" -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE -ne 0) {
    throw 'Visual Studio developer environment initialization failed.'
}
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable(
            $line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
    }
}

Push-Location -LiteralPath $projectRoot
try {
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "Configure failed: $LASTEXITCODE" }
    & cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
    if ($Preset -ne 'release') {
        if ($TestFilter) {
            & (Join-Path $projectRoot "build/$Preset/lsm_tests.exe") "--gtest_filter=$TestFilter"
        } else {
            & ctest --preset $Preset --verbose
        }
        if ($LASTEXITCODE -ne 0) { throw "Tests failed: $LASTEXITCODE" }
    }
} finally {
    Pop-Location
}
