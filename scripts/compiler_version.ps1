$ErrorActionPreference = 'Stop'
$version = (Get-Item -LiteralPath (Get-Command cl.exe).Source).VersionInfo.FileVersion
if ($version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    throw 'MSVC executable has no numeric file version.'
}
"MSVC $version"
