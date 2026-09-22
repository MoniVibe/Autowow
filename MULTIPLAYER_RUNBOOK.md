# AutoWoW local multiplayer runbook

The current server intentionally remains loopback-only. It supports multiple real player accounts, but it is not exposed to a LAN or the Internet.

## Add another local player

Use a new PowerShell window and supply secrets only to that process:

```powershell
$env:WOW_ACCOUNT_NAME = 'PLAYER_TWO'
$env:WOW_ACCOUNT_PASSWORD = [System.Net.NetworkCredential]::new('', (Read-Host 'Player password' -AsSecureString)).Password
$env:MYSQL_ROOT_PASSWORD = [System.Net.NetworkCredential]::new('', (Read-Host 'MySQL root password' -AsSecureString)).Password
.\scripts\provision-local-player.ps1
```

`MYSQL_ROOT_PASSWORD` must be a plaintext environment value for the bundled MySQL client. Do not place it in a script, profile, report, or Git file. After provisioning, remove it from the current process:

```powershell
Remove-Item Env:MYSQL_ROOT_PASSWORD
Remove-Item Env:WOW_ACCOUNT_PASSWORD
```

Each player starts the copied 3.3.5a client and logs in through `127.0.0.1`. The realm remains `AutoWoW Local`.

## Character creation and Playerbots

Character creation is performed from the normal 3.3.5a client character screen. The current server starts with five conservative random bots. The loopback AutoWow bridge can pause, resume, inspect, and send those random bots to a level-appropriate location. Follow, direct attack, and loot orders remain the next bridge milestone and are not claimed as implemented.

## LAN access is a deliberate future step

Before enabling another machine, choose a private LAN address and explicitly update the realm address, server bind configuration, each client realmlist, and Windows Firewall rules for the Private profile only. MySQL (`3306`) must remain loopback-only. Never forward World of Warcraft ports to the Internet in this development setup.

No LAN firewall or bind change has been made by the overnight work.
