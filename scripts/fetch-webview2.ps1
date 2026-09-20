# fetch-webview2.ps1 - Download the WebView2 SDK payload for a local build.
#
# engine/CMakeLists.txt reads tools/webview2/build/native (WebView2.h plus
# WebView2LoaderStatic.lib). The SDK is not committed: it is 12 MB of NuGet
# package files, and only the build/native folder is used. CI runs this same
# script, so the version below is the single source of truth.

$ErrorActionPreference = 'Stop'
$ver  = '1.0.4191.47'
$repo = Split-Path $PSScriptRoot
$zip  = Join-Path $env:TEMP 'wv2.zip'
$pkg  = Join-Path $env:TEMP 'wv2pkg'

Write-Host "Fetching Microsoft.Web.WebView2 $ver from nuget.org..."
Invoke-WebRequest "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$ver" -OutFile $zip
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
Expand-Archive $zip $pkg

New-Item -ItemType Directory -Force "$repo\tools\webview2\build" | Out-Null
Copy-Item -Recurse -Force "$pkg\build\native" "$repo\tools\webview2\build\"

Write-Host "WebView2 SDK $ver -> tools\webview2\build\native"
