# publish.ps1 - Create the GitHub repo and push (run after: gh auth login)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$gh = "$repo\tools\gh\bin\gh.exe"

& $gh auth status
if ($LASTEXITCODE -ne 0) { throw 'Not logged in. Run: tools\gh\bin\gh.exe auth login' }

& $gh repo create ChickenD233/FPS-SoundRadar --public --source=$repo --push `
    --description "Game-audio direction radar + right-ear mono downmix for one-sided hearing loss (Valorant/Delta Force/CS2). sysvad-based virtual 7.1 driver + WASAPI engine + D2D overlay."
if ($LASTEXITCODE -ne 0) { throw "repo create/push failed ($LASTEXITCODE)" }

& $gh repo edit ChickenD233/FPS-SoundRadar --add-topic audio --add-topic accessibility --add-topic fps --add-topic wasapi --add-topic windows-driver --add-topic hearing-loss
Write-Host 'Published: https://github.com/ChickenD233/FPS-SoundRadar'
