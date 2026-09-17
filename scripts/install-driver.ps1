# install-driver.ps1 - Test-sign and install the SoundRadar VAD driver (DEVELOPMENT ONLY)
#
# WARNING: This script enables test mode. Vanguard (Valorant) and ACE (Delta Force)
# block games while test mode is on. Use a separate PC or VM for development.
# For daily gaming, use attestation signing. See docs/signing.md.
#
# Run from an elevated PowerShell:  powershell -ExecutionPolicy Bypass -File install-driver.ps1

#Requires -RunAsAdministrator
param(
    [string]$PackageDir = "$PSScriptRoot\..\build\driver\Package",
    [switch]$SkipRebootPrompt
)
$ErrorActionPreference = 'Stop'

Write-Host "=== SoundRadar VAD test-sign installer ==="

$certSubject = 'CN=SoundRadarTestCert'
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe' | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
$inf2cat  = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x86\inf2cat.exe'  | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $signtool -or -not $inf2cat) { throw 'WDK tools not found. Install the WDK first (see README).' }

# 1. Create the test certificate once.
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq $certSubject | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Subject $certSubject -Type CodeSigningCert `
        -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(5)
    Write-Host "[1/6] Created certificate $certSubject"
} else { Write-Host "[1/6] Certificate $certSubject already exists" }

# 2. Trust the certificate on this machine.
$store Root, TrustedPublisher | ForEach-Object {
    $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($_, 'LocalMachine')
    $s.Open('ReadWrite'); $s.Add($cert); $s.Close()
}
Write-Host '[2/6] Certificate trusted (Root + TrustedPublisher)'

# 3. Sign the driver binary and the catalog that build-driver.ps1 generated.
$cat = Get-ChildItem $PackageDir -Filter *.cat | Select-Object -First 1
$sys = Get-ChildItem $PackageDir -Filter *.sys | Select-Object -First 1
$inf = Get-ChildItem $PackageDir -Filter *.inf | Select-Object -First 1
if (-not $sys -or -not $cat -or -not $inf) { throw "Package incomplete in $PackageDir. Run scripts/build-driver.ps1 first." }
& $signtool sign /fd sha256 /td sha256 /tr http://timestamp.digicert.com /s My /n $certSubject $sys.FullName | Out-Null
& $signtool sign /fd sha256 /s My /n $certSubject $cat.FullName | Out-Null
Write-Host "[3/6] Signed $($sys.Name) and $($cat.Name)"

# 4. Enable test mode.
bcdedit /set testsigning on | Out-Null
Write-Host '[4/6] Test mode ON (takes effect after reboot)'

# 5. Install the driver package.
pnputil /add-driver $inf.FullName /install
Write-Host '[5/6] Driver package staged and install triggered'

# 6. Reboot is required.
Write-Host '[6/6] Done. A reboot is required before the device appears.'
if (-not $SkipRebootPrompt) {
    $r = Read-Host 'Reboot now? [y/N]'
    if ($r -eq 'y') { shutdown /r /t 5 }
}
