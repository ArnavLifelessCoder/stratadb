param(
    [string]$Version = '',
    [switch]$GitHubActions
)

$ErrorActionPreference = 'Stop'
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswherePath)) {
    throw 'Install Visual Studio Build Tools with the Desktop development with C++ workload.'
}
$arguments = @('-latest', '-products', '*', '-requires',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
if ($Version) {
    $arguments += @('-version', $Version)
}
$vsInstall = & $vswherePath @arguments
if (-not $vsInstall) { throw 'No matching Visual Studio C++ toolchain was found.' }
$developerCommand = Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat'
$environmentLines = & $env:ComSpec /d /s /c "`"$developerCommand`" -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment initialization failed.' }
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        $variableName = $line.Substring(0, $separator)
        $variableValue = $line.Substring($separator + 1)
        $previousValue = [Environment]::GetEnvironmentVariable($variableName, 'Process')
        if ($GitHubActions -and $previousValue -ne $variableValue) {
            Add-Content -LiteralPath $env:GITHUB_ENV -Value $line -Encoding utf8
        }
        [Environment]::SetEnvironmentVariable($variableName, $variableValue, 'Process')
    }
}
