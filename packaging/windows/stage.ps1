# SPDX-License-Identifier: Apache-2.0
# Collects a Windows build into a self-contained folder for the installer:
#   openmine-desktop.exe + every DLL it needs (vcpkg app-local copies, Qt via windeployqt,
#   the MSVC runtime), pdal-plugins\, plugins\ (samples), docs and licence texts.
#
#   pwsh packaging/windows/stage.ps1 -Build build/windows-msvc -Out stage
param(
    [Parameter(Mandatory = $true)][string]$Build,
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = "Stop"
$Src = Resolve-Path "$PSScriptRoot/../.."
$Build = Resolve-Path $Build
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out, "$Out/pdal-plugins", "$Out/plugins", "$Out/docs", "$Out/licenses" | Out-Null

$Bin = Join-Path $Build "bin"
$Exe = Join-Path $Bin "openmine-desktop.exe"
if (-not (Test-Path $Exe)) { throw "No $Exe — build first" }
$Vcpkg = Join-Path $Build "vcpkg_installed/x64-windows"

# The application and the DLLs vcpkg copied next to it.
Copy-Item "$Bin/*.exe", "$Bin/*.dll" -Destination $Out
# Every DLL of the vcpkg tree the application may load indirectly (PDAL, GDAL drivers' deps, …).
Get-ChildItem "$Vcpkg/bin" -Filter *.dll | Where-Object { $_.Name -notlike "libpdal_plugin_*" } |
    ForEach-Object { if (-not (Test-Path (Join-Path $Out $_.Name))) { Copy-Item $_.FullName $Out } }
# PDAL format plugins (the application points PDAL_DRIVER_PATH at pdal-plugins\).
Get-ChildItem "$Vcpkg/bin" -Filter "libpdal_plugin_*.dll" -ErrorAction SilentlyContinue | Copy-Item -Destination "$Out/pdal-plugins"
# PROJ and GDAL data files.
New-Item -ItemType Directory -Force -Path "$Out/share" | Out-Null
foreach ($d in @("proj", "gdal")) {
    $data = Join-Path $Vcpkg "share/$d"
    if (Test-Path $data) { Copy-Item -Recurse $data "$Out/share/$d" }
}

# Qt: platform, style, image-format and SVG plugins.
$Windeployqt = Get-ChildItem -Recurse -Path "$Vcpkg/tools" -Filter windeployqt.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $Windeployqt) { throw "windeployqt.exe not found under $Vcpkg/tools" }
& $Windeployqt.FullName --release --no-translations --no-system-d3d-compiler --no-opengl-sw (Join-Path $Out "openmine-desktop.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

# MSVC runtime, app-local (no separate redistributable needed).
$Crt = Get-ChildItem "$env:VCToolsRedistDir/x64" -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($Crt) { Copy-Item "$($Crt.FullName)/*.dll" $Out } else { Write-Warning "MSVC runtime not found (VCToolsRedistDir); the installer will need the VC++ redistributable" }

# Sample plugins, documentation, licences.
# Only the sample plugins: the build's plugins folder also holds the test suite's plugins.
foreach ($sample in Get-ChildItem -Directory "$Src/plugins/samples") {
    $built = Join-Path $Build "plugins/$($sample.Name)"
    if ((Test-Path "$built/plugin.json") -and (Get-ChildItem $built -Filter *.dll)) {
        Copy-Item -Recurse $built "$Out/plugins/$($sample.Name)"
    }
}
if (Test-Path "$Build/python/openmine") {   # Python plugins only work in a build with the Python API
    Copy-Item -Recurse "$Build/plugins/python" "$Out/plugins/python"
}
Copy-Item "$Src/LICENSE", "$Src/NOTICE", "$Src/THIRD_PARTY_NOTICES.md", "$Src/README.md" "$Out/docs"
Copy-Item "$Src/docs/user-guide.md", "$Src/docs/python.md", "$Src/docs/plugins.md", "$Src/docs/console-commands.md" "$Out/docs"
Copy-Item "$Src/packaging/linux/licenses/*" "$Out/licenses"
Get-ChildItem "$Vcpkg/share" -Recurse -Filter copyright | ForEach-Object {
    Copy-Item $_.FullName (Join-Path "$Out/licenses" ("{0}.txt" -f $_.Directory.Name))
}
Write-Host "Staged $((Get-ChildItem -Recurse $Out | Measure-Object).Count) files in $Out"
