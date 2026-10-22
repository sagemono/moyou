# Package a Windows build into dist\moyou-<version>-win64.zip (moyou.exe, moyou-cli.exe, Qt runtime, README, USAGE).
param(
    [string]$Preset = "windows",
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Preset\$Config"
if (-not (Test-Path "$bin\moyou.exe")) { throw "No build at $bin. Run build.bat first." }

$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'VERSION (\d+\.\d+\.\d+)' | Select-Object -First 1).Matches[0].Groups[1].Value
$name = "moyou-$version-win64"
$dist = Join-Path $root "dist\$name"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null

Copy-Item "$bin\moyou.exe", "$bin\moyou-cli.exe" $dist
Get-ChildItem $bin -Filter *.dll | Copy-Item -Destination $dist
foreach ($d in "platforms", "styles", "imageformats") {
    if (Test-Path "$bin\$d") { Copy-Item -Recurse "$bin\$d" $dist }
}
Copy-Item "$root\README.md", "$root\docs\USAGE.md", "$root\LICENSE", "$root\THIRD-PARTY-NOTICES.md" $dist

# The licence of every library in the zip: vcpkg keeps each port's in share\<port>\copyright.
$installed = @("$root\build\$Preset\vcpkg_installed\x64-windows", "$env:VCPKG_ROOT\installed\x64-windows") |
    Where-Object { Test-Path "$_\share" } | Select-Object -First 1
if (-not $installed) { throw "No vcpkg installed tree found for the licence files." }
New-Item -ItemType Directory -Force "$dist\licenses" | Out-Null
Get-ChildItem "$installed\share" -Directory | ForEach-Object {
    if (Test-Path "$($_.FullName)\copyright") { Copy-Item "$($_.FullName)\copyright" "$dist\licenses\$($_.Name).txt" }
}

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$dist\*" -DestinationPath $zip
Write-Host "Packaged $zip"
