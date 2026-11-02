# Collects the built DLL, the INI and the panel's movie into a Vortex-ready zip:
#   FastTravelArrivalPlanner-mod.zip
#     SKSE\Plugins\FastTravelArrivalPlanner.dll
#     SKSE\Plugins\FastTravelArrivalPlanner.ini
#     Interface\FastTravelArrivalPlanner.swf
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'build'

$dll = Get-ChildItem -Path $buildDir -Recurse -Filter 'FastTravelArrivalPlanner.dll' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $dll) {
    Write-Host 'FastTravelArrivalPlanner.dll was not found in the build folder.'
    exit 1
}

$stage = Join-Path $root 'mod'
if (Test-Path $stage) {
    Remove-Item $stage -Recurse -Force
}
$plugins = Join-Path (Join-Path $stage 'SKSE') 'Plugins'
New-Item -ItemType Directory -Force -Path $plugins | Out-Null
Copy-Item $dll.FullName $plugins
Copy-Item (Join-Path (Join-Path (Join-Path (Join-Path $root 'dist') 'SKSE') 'Plugins') 'FastTravelArrivalPlanner.ini') $plugins
$interface = Join-Path $stage 'Interface'
New-Item -ItemType Directory -Force -Path $interface | Out-Null
Copy-Item (Join-Path (Join-Path (Join-Path $root 'dist') 'Interface') 'FastTravelArrivalPlanner.swf') $interface

$zip = Join-Path $root 'FastTravelArrivalPlanner-mod.zip'
if (Test-Path $zip) {
    Remove-Item $zip -Force
}
Compress-Archive -Path (Join-Path $stage 'SKSE'), $interface -DestinationPath $zip

Write-Host "Created $zip"
exit 0
