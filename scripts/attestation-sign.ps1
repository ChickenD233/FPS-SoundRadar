# attestation-sign.ps1 - EV-sign the driver package and prepare it for Microsoft attestation signing.
# Use this output for the machine where you play Valorant / Delta Force / CS2.
#
# Before you run this script:
# 1. Buy an EV code-signing certificate (DigiCert, Sectigo, GlobalSign). It arrives on a USB token.
# 2. Install the token middleware so the certificate shows up in Cert:\CurrentUser\My.
# 3. Register a free Partner Center account at https://partner.microsoft.com/dashboard/hardware
#    and complete the legal agreements (browser step).

#Requires -RunAsAdministrator
param(
    [string]$PackageDir = "$PSScriptRoot\..\build\driver\Package",
    [string]$EvCertSubject  # e.g. 'CN=Your Company Name'
)
$ErrorActionPreference = 'Stop'
if (-not $EvCertSubject) { throw 'Pass -EvCertSubject with the subject of your EV certificate.' }

$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe' | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
$inf2cat  = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\inf2cat.exe'  | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName

# 1. Build the catalog and sign everything with the EV certificate.
& $inf2cat /driver:$PackageDir /os:10_X64 /uselocaltime | Out-Null
Get-ChildItem $PackageDir -Include *.sys, *.cat -Recurse | ForEach-Object {
    & $signtool sign /fd sha256 /td sha256 /tr http://timestamp.digicert.com /s My /n $EvCertSubject $_.FullName | Out-Null
}
Write-Host 'EV signature applied.'

# 2. Zip the package for submission.
$zip = "$PackageDir\..\SoundRadarVAD-submission.zip"
Compress-Archive -Path "$PackageDir\*" -DestinationPath $zip -Force
Write-Host "Submission package: $zip"

# 3. Browser step: submit for attestation signing.
Write-Host ''
Write-Host 'Next (browser):'
Write-Host ' 1. Open https://partner.microsoft.com/dashboard/hardware'
Write-Host ' 2. Go to Drivers > New driver.'
Write-Host " 3. Upload $zip and choose attestation signing."
Write-Host ' 4. Download the co-signed package, unzip it over build\driver\Package.'
Write-Host ' 5. Install with: pnputil /add-driver <inf> /install  (no test mode needed)'
