# uninstall-driver.ps1 - Remove the SoundRadar VAD driver and the test certificate.
# Run from an elevated PowerShell.

#Requires -RunAsAdministrator
param([switch]$DisableTestMode)
$ErrorActionPreference = 'Stop'

# Remove device nodes that use the SoundRadar HWID, then remove the driver package.
$devs = Get-PnpDevice | Where-Object { $_.InstanceId -like 'ROOT\SoundRadarVAD*' }
foreach ($d in $devs) { pnputil /remove-device $d.InstanceId }
$infs = pnputil /enum-drivers | Select-String -Pattern 'oem\d+\.inf' -Context 0,10 |
    Where-Object { $_.Context.PostContext -match 'SoundRadar' } |
    ForEach-Object { $_.Matches.Value }
foreach ($oem in $infs | Select-Object -Unique) { pnputil /delete-driver $oem /uninstall /force }
Write-Host 'Driver removed.'

# Remove the test certificate.
Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq 'CN=SoundRadarTestCert' | Remove-Item
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq 'CN=SoundRadarTestCert' | Remove-Item
}
Write-Host 'Certificate removed.'

if ($DisableTestMode) {
    bcdedit /set testsigning off | Out-Null
    Write-Host 'Test mode OFF. Reboot to apply.'
}
Write-Host 'Reboot to finish.'
