# build-driver.ps1 - Build and package the SoundRadar VAD driver (Release x64).
# Needs: VS2022 BuildTools (C++ workload) + Windows SDK 10.0.26100 + WDK 10.0.26100.6584.
# Run from any shell:  powershell -ExecutionPolicy Bypass -File scripts/build-driver.ps1

$ErrorActionPreference = 'Stop'
$repo    = Split-Path $PSScriptRoot
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
$wdkBin  = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0'
$out     = "$repo\build\driver\Package"

if (-not (Test-Path $msbuild)) { throw "MSBuild not found at $msbuild" }
if (-not (Test-Path 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km\wdm.h')) { throw 'WDK not installed. See docs/signing.md and README.' }

# 1. Install the WindowsKernelModeDriver10.0 toolset glue if the WDK VSIX is absent.
#    (BuildTools-only machines: the WDK VSIX does not install, so we ship the glue.)
$glueDir = "$env:ProgramFiles (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Microsoft\VC\v170\Platforms\x64"
$glueDir = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Microsoft\VC\v170\Platforms\x64"
if (-not (Test-Path "$glueDir\PlatformToolsets\WindowsKernelModeDriver10.0\Toolset.props")) {
    Write-Host 'Installing driver toolset glue (needs admin rights once)...'
    New-Item -ItemType Directory -Force "$glueDir\PlatformToolsets\WindowsKernelModeDriver10.0" | Out-Null
    New-Item -ItemType Directory -Force "$glueDir\ImportAfter" | Out-Null
    Copy-Item "$repo\driver\toolset-glue\Toolset.props", "$repo\driver\toolset-glue\Toolset.targets" "$glueDir\PlatformToolsets\WindowsKernelModeDriver10.0"
    Copy-Item "$repo\driver\toolset-glue\WDK.x64.*", "$repo\driver\toolset-glue\Dbgeng*" "$glueDir\ImportAfter"
}

# 2. Build the static library, then the driver.
& $msbuild "$repo\driver\sysvad\EndpointsCommon\EndpointsCommon.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:m
if ($LASTEXITCODE -ne 0) { throw "EndpointsCommon build failed ($LASTEXITCODE)" }
& $msbuild "$repo\driver\sysvad\TabletAudioSample\TabletAudioSample.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:m
if ($LASTEXITCODE -ne 0) { throw "TabletAudioSample build failed ($LASTEXITCODE)" }

# 3. Stage the package: stamp the INF, copy the .sys, generate the catalog.
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$repo\driver\sysvad\TabletAudioSample\ComponentizedAudioSample.inx" "$out\SoundRadarVAD.inf" -Force
& "$wdkBin\x64\stampinf.exe" -f "$out\SoundRadarVAD.inf" -d (Get-Date -Format 'MM\/dd\/yyyy') -a amd64 -k 1.15 -v 1.0.0.0
if ($LASTEXITCODE -ne 0) { throw "stampinf failed ($LASTEXITCODE)" }
Copy-Item "$repo\driver\sysvad\TabletAudioSample\x64\Release\TabletAudioSample.sys" $out -Force
& "$wdkBin\x86\Inf2Cat.exe" /driver:$out /os:10_X64 /uselocaltime
if ($LASTEXITCODE -ne 0) { throw "inf2cat failed ($LASTEXITCODE)" }

Write-Host ''
Write-Host "Driver package ready in $out :"
Get-ChildItem $out
Write-Host ''
Write-Host 'Next: scripts\install-driver.ps1 (test signing, dev PCs only)'
Write-Host '   or scripts\attestation-sign.ps1 (for gaming PCs with Vanguard/ACE)'
