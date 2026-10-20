$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $root 'scripts/msvc_environment.ps1') -Version '[17.0,18.0)'
$version = & (Join-Path $root 'scripts/compiler_version.ps1')
if ($version -notmatch '^MSVC \d+\.\d+\.\d+\.\d+$') {
    throw "Expected a numeric MSVC compiler version; got: $version"
}
Write-Output "Compiler metadata regression passed: $version"
