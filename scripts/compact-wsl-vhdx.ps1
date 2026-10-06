# Compacts the Ubuntu-24.04 WSL disk after in-distro cleanup + fstrim.
# Run in an ELEVATED (Administrator) PowerShell. Stops WSL first.
$vhdx = 'C:\Users\Moni\AppData\Local\wsl\Ubuntu-24.04-compact\ext4.vhdx'
$before = [math]::Round((Get-Item $vhdx).Length / 1GB, 1)
wsl.exe --shutdown
Start-Sleep -Seconds 8
$script = @"
select vdisk file="$vhdx"
attach vdisk readonly
compact vdisk
detach vdisk
exit
"@
$tmp = Join-Path $env:TEMP 'compact-wsl.txt'
Set-Content -LiteralPath $tmp -Value $script -Encoding ASCII
diskpart /s $tmp
Remove-Item -LiteralPath $tmp
$after = [math]::Round((Get-Item $vhdx).Length / 1GB, 1)
"ext4.vhdx: $before GB -> $after GB"
