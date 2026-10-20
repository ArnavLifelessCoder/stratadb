param(
    [string]$Milestone = 'm4',
    [switch]$CleanEvidence
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$resultDirectory = Join-Path $projectRoot "results/$Milestone"
if ($CleanEvidence) {
    $resultDirectory = Join-Path $projectRoot "build/verification-$Milestone"
    $initialStatus = @(& git -C $projectRoot status --porcelain)
    if ($initialStatus.Count -ne 0) { throw 'Clean evidence requires a clean committed tree' }
}
New-Item -ItemType Directory -Force -Path $resultDirectory | Out-Null

Push-Location -LiteralPath $projectRoot
try {
    $metadata = [ordered]@{
        recorded_utc = [DateTime]::UtcNow.ToString('o')
        platform = 'Windows'
        os = (Get-CimInstance Win32_OperatingSystem).Caption
        os_version = [Environment]::OSVersion.Version.ToString()
        architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
        logical_processors = [Environment]::ProcessorCount
        source_commit = (& git rev-parse HEAD)
        source_status = @(& git status --short)
        presets = @('debug', 'asan')
        result = 'running'
    }
    $metadataPath = Join-Path $resultDirectory 'windows.json'
    $metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $metadataPath -Encoding utf8
    foreach ($preset in @('debug', 'asan')) {
        & (Join-Path $PSScriptRoot 'build.ps1') -Preset $preset *>&1 |
            Tee-Object -FilePath (Join-Path $resultDirectory "windows-$preset.log")
    }
    $metadata.result = 'passed'
    $metadata.compiler = & (Join-Path $PSScriptRoot 'compiler_version.ps1')
    $metadata.cmake = (& cmake --version | Select-Object -First 1).ToString()
    $metadata.ninja = (& ninja --version).ToString()
    if ($CleanEvidence) {
        $finalStatus = @(& git status --porcelain)
        if ($finalStatus.Count -ne 0 -or (& git rev-parse HEAD) -ne $metadata.source_commit) {
            throw 'Source changed during clean evidence collection'
        }
    }
    $metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $metadataPath -Encoding utf8
} finally {
    Pop-Location
}
