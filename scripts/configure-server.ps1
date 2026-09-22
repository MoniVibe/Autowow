[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('127.0.0.1','::1','localhost')][string]$BindIP = '127.0.0.1',
    [string]$DataDir = '',
    [int]$RandomBots = 5
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Assert-LocalOnlyAddress -Address $BindIP
if ($RandomBots -lt 0 -or $RandomBots -gt 80) { throw 'RandomBots must be between 0 and 80.' }
if ([string]::IsNullOrWhiteSpace($DataDir)) { $DataDir = Join-Path $ServerRoot 'server\data' }
$DataDir = [System.IO.Path]::GetFullPath($DataDir)

$installRoot = Join-Path $ServerRoot 'server'
$coreRoot = Join-Path $ServerRoot 'azerothcore-wotlk'
$configRoot = Join-Path $installRoot 'configs'
$moduleConfigRoot = Join-Path $configRoot 'modules'
Initialize-AutoWoWLayout -Root $ServerRoot
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null
New-Item -ItemType Directory -Path $configRoot -Force | Out-Null
New-Item -ItemType Directory -Path $moduleConfigRoot -Force | Out-Null

function Ensure-Config {
    param([string]$Destination, [string]$Source)
    if (-not (Test-Path -LiteralPath $Destination)) {
        if (-not (Test-Path -LiteralPath $Source)) { throw "Config source missing: $Source" }
        Copy-Item -LiteralPath $Source -Destination $Destination
    }
}

$authConfig = Join-Path $configRoot 'authserver.conf'
$worldConfig = Join-Path $configRoot 'worldserver.conf'
$dbConfig = Join-Path $configRoot 'dbimport.conf'
$playerbotConfig = Join-Path $moduleConfigRoot 'playerbots.conf'
Ensure-Config -Destination $authConfig -Source (Join-Path $coreRoot 'src\server\apps\authserver\authserver.conf.dist')
Ensure-Config -Destination $worldConfig -Source (Join-Path $coreRoot 'src\server\apps\worldserver\worldserver.conf.dist')
Ensure-Config -Destination $dbConfig -Source (Join-Path $coreRoot 'src\tools\dbimport\dbimport.conf.dist')
Ensure-Config -Destination $playerbotConfig -Source (Join-Path $coreRoot 'modules\mod-playerbots\conf\playerbots.conf.dist')

foreach ($config in @($authConfig, $worldConfig, $dbConfig, $playerbotConfig)) {
    $backup = "$config.bootstrap.bak"
    if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $config -Destination $backup }
}

Set-ConfigValue -Path $authConfig -Key 'BindIP' -Value '"127.0.0.1"'
Set-ConfigValue -Path $authConfig -Key 'LogsDir' -Value '"logs"'
Set-ConfigValue -Path $authConfig -Key 'LoginDatabaseInfo' -Value '"127.0.0.1;3306;acore;acore;acore_auth"'
Set-ConfigValue -Path $authConfig -Key 'RealmServerPort' -Value '3724'

Set-ConfigValue -Path $worldConfig -Key 'BindIP' -Value '"127.0.0.1"'
Set-ConfigValue -Path $worldConfig -Key 'LogsDir' -Value '"logs"'
Set-ConfigValue -Path $worldConfig -Key 'WorldServerPort' -Value '8085'
Set-ConfigValue -Path $worldConfig -Key 'LoginDatabaseInfo' -Value '"127.0.0.1;3306;acore;acore;acore_auth"'
Set-ConfigValue -Path $worldConfig -Key 'WorldDatabaseInfo' -Value '"127.0.0.1;3306;acore;acore;acore_world"'
Set-ConfigValue -Path $worldConfig -Key 'CharacterDatabaseInfo' -Value '"127.0.0.1;3306;acore;acore;acore_characters"'
Set-ConfigValue -Path $worldConfig -Key 'DataDir' -Value ('"' + $DataDir.Replace('\','/') + '"')
Set-ConfigValue -Path $worldConfig -Key 'MapUpdate.Threads' -Value '4'

$mysqlRoot = Get-FirstExistingPath -Candidates @(
    $env:MYSQL_ROOT_DIR,
    (Join-Path $ServerRoot 'third_party\mysql'),
    'C:\Program Files\MySQL\MySQL Server 8.4',
    'C:\Program Files\MySQL\MySQL Server 8.0'
)
$mysqlExe = if ($mysqlRoot) { Get-FirstExistingPath -Candidates @((Join-Path $mysqlRoot 'bin\mysql.exe')) } else { $null }
Set-ConfigValue -Path $dbConfig -Key 'LogsDir' -Value '"logs"'
Set-ConfigValue -Path $dbConfig -Key 'SourceDirectory' -Value ('"' + $coreRoot.Replace('\','/') + '"')
Set-ConfigValue -Path $dbConfig -Key 'MySQLExecutable' -Value ('"' + $(if ($mysqlExe) { $mysqlExe.Replace('\','/') } else { 'mysql.exe' }) + '"')
Set-ConfigValue -Path $dbConfig -Key 'TempDir' -Value '"temp"'
Set-ConfigValue -Path $dbConfig -Key 'Updates.AllowedModules' -Value '"all"'
Set-ConfigValue -Path $dbConfig -Key 'Updates.AutoSetup' -Value '1'

Set-ConfigValue -Path $playerbotConfig -Key 'AiPlayerbot.Enabled' -Value '1'
Set-ConfigValue -Path $playerbotConfig -Key 'AiPlayerbot.RandomBotAutologin' -Value '1'
Set-ConfigValue -Path $playerbotConfig -Key 'AiPlayerbot.MinRandomBots' -Value "$RandomBots"
Set-ConfigValue -Path $playerbotConfig -Key 'AiPlayerbot.MaxRandomBots' -Value "$RandomBots"
Set-ConfigValue -Path $playerbotConfig -Key 'AiPlayerbot.AddClassAccountPoolSize' -Value '1'
Set-ConfigValue -Path $playerbotConfig -Key 'PlayerbotsDatabaseInfo' -Value '"127.0.0.1;3306;acore;acore;acore_playerbots"'
Set-ConfigValue -Path $playerbotConfig -Key 'AiPlayerbot.CommandServerPort' -Value '0'
Set-ConfigValue -Path $playerbotConfig -Key 'AutoWow.BridgePort' -Value '18787'

Write-Output "Configured localhost server in $installRoot"
Write-Output "DataDir: $DataDir"
Write-Output "Random bots: $RandomBots"
