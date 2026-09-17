@echo off
rem 在桌面创建 SoundRadar 快捷方式
powershell -NoProfile -Command "$ws = New-Object -ComObject WScript.Shell; $sc = $ws.CreateShortcut([Environment]::GetFolderPath('Desktop') + '\SoundRadar.lnk'); $sc.TargetPath = '%~dp0SoundRadar.exe'; $sc.WorkingDirectory = '%~dp0'; $sc.Description = 'SoundRadar 声纹雷达'; $sc.Save()"
echo 快捷方式已创建到桌面
pause
