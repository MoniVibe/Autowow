Set-StrictMode -Version Latest

$script:OracleRaceSeedDefinitions = @(
    [pscustomobject]@{ guid = [uint32]101; race_id = 4; race_name = 'Night Elf'; faction = 'Alliance'; class_id = 11; class_name = 'Druid'; class_plan = 'balance'; profession_one = 'Herbalism'; profession_two = 'Alchemy'; starting_map = 1; starting_zone = 141; name = 'Zathis' }
    [pscustomobject]@{ guid = [uint32]112; race_id = 1; race_name = 'Human'; faction = 'Alliance'; class_id = 1; class_name = 'Warrior'; class_plan = 'protection'; profession_one = 'Mining'; profession_two = 'Blacksmithing'; starting_map = 0; starting_zone = 12; name = 'Naomini' }
    [pscustomobject]@{ guid = [uint32]121; race_id = 6; race_name = 'Tauren'; faction = 'Horde'; class_id = 11; class_name = 'Druid'; class_plan = 'feral'; profession_one = 'Herbalism'; profession_two = 'Skinning'; starting_map = 1; starting_zone = 215; name = 'Musliwho' }
    [pscustomobject]@{ guid = [uint32]123; race_id = 10; race_name = 'Blood Elf'; faction = 'Horde'; class_id = 2; class_name = 'Paladin'; class_plan = 'retribution'; profession_one = 'Mining'; profession_two = 'Jewelcrafting'; starting_map = 530; starting_zone = 3430; name = 'Aenstus' }
    [pscustomobject]@{ guid = [uint32]139; race_id = 7; race_name = 'Gnome'; faction = 'Alliance'; class_id = 8; class_name = 'Mage'; class_plan = 'frost'; profession_one = 'Mining'; profession_two = 'Engineering'; starting_map = 0; starting_zone = 1; name = 'Bitlas' }
    [pscustomobject]@{ guid = [uint32]144; race_id = 8; race_name = 'Troll'; faction = 'Horde'; class_id = 3; class_name = 'Hunter'; class_plan = 'beast_mastery'; profession_one = 'Herbalism'; profession_two = 'Alchemy'; starting_map = 1; starting_zone = 14; name = 'Tzigdan' }
    [pscustomobject]@{ guid = [uint32]154; race_id = 3; race_name = 'Dwarf'; faction = 'Alliance'; class_id = 3; class_name = 'Hunter'; class_plan = 'marksmanship'; profession_one = 'Mining'; profession_two = 'Engineering'; starting_map = 0; starting_zone = 1; name = 'Khirgegs' }
    [pscustomobject]@{ guid = [uint32]166; race_id = 11; race_name = 'Draenei'; faction = 'Alliance'; class_id = 5; class_name = 'Priest'; class_plan = 'shadow'; profession_one = 'Herbalism'; profession_two = 'Inscription'; starting_map = 530; starting_zone = 3524; name = 'Valrula' }
    [pscustomobject]@{ guid = [uint32]236; race_id = 5; race_name = 'Undead'; faction = 'Horde'; class_id = 5; class_name = 'Priest'; class_plan = 'shadow'; profession_one = 'Herbalism'; profession_two = 'Tailoring'; starting_map = 0; starting_zone = 85; name = 'Nolliano' }
    [pscustomobject]@{ guid = [uint32]244; race_id = 2; race_name = 'Orc'; faction = 'Horde'; class_id = 3; class_name = 'Hunter'; class_plan = 'beast_mastery'; profession_one = 'Skinning'; profession_two = 'Leatherworking'; starting_map = 1; starting_zone = 14; name = 'Ugrilok' }
)

function Get-OracleRaceSeedDefinitions {
    return @($script:OracleRaceSeedDefinitions)
}

function Get-OracleRaceSeedByGuid {
    param([Parameter(Mandatory = $true)][uint32]$Guid)
    return @($script:OracleRaceSeedDefinitions | Where-Object { [uint32]$_.guid -eq $Guid } | Select-Object -First 1)
}

function Get-OracleRaceSeedGuidList {
    return @($script:OracleRaceSeedDefinitions | Sort-Object guid | ForEach-Object { [uint32]$_.guid })
}

function Get-OracleProfessionSkillIds {
    return [ordered]@{
        Herbalism = 182
        Mining = 186
        Skinning = 393
        Alchemy = 171
        Blacksmithing = 164
        Engineering = 202
        Jewelcrafting = 755
        Inscription = 773
        Leatherworking = 165
        Tailoring = 197
    }
}

function Get-OracleProfessionPlanAssessment {
    param(
        [Parameter(Mandatory = $true)][object]$Definition,
        [AllowNull()][object]$Telemetry,
        [string]$Source = 'bridge.professioneconomy'
    )

    $desired = @([string]$Definition.profession_one, [string]$Definition.profession_two)
    $desired = @($desired | ForEach-Object { $_.Trim().ToLowerInvariant() })
    $observed = @()
    $source = 'unavailable'
    if ($null -ne $Telemetry) {
        $source = $Source
        $observed = @((Get-OptionalProperty -InputObject $Telemetry -Name 'professions' -Default @()) |
            ForEach-Object { ([string](Get-OptionalProperty -InputObject $_ -Name 'name' -Default '')).Trim().ToLowerInvariant() } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Sort-Object -Unique)
    }

    $missing = @($desired | Where-Object { $_ -notin $observed })
    $status = if ($null -eq $Telemetry) {
        'unavailable'
    }
    elseif ($missing.Count -eq 0) {
        'realized'
    }
    else {
        'not_realized'
    }

    return [ordered]@{
        status = $status
        source = $source
        desired = @($desired)
        observed = @($observed)
        missing = @($missing)
    }
}

function Assert-OracleRaceSeedDefinitions {
    $definitions = @(Get-OracleRaceSeedDefinitions)
    if ($definitions.Count -ne 10) { throw "Expected ten WotLK playable race seeds, found $($definitions.Count)." }
    $guids = @($definitions | ForEach-Object { [uint32]$_.guid })
    $races = @($definitions | ForEach-Object { [int]$_.race_id })
    if (@($guids | Sort-Object -Unique).Count -ne $guids.Count) { throw 'Race seed GUIDs are not unique.' }
    if (@($races | Sort-Object -Unique).Count -ne $races.Count) { throw 'Race seed race IDs are not unique.' }
    foreach ($definition in $definitions) {
        if ([string]::IsNullOrWhiteSpace([string]$definition.profession_one) -or
            [string]::IsNullOrWhiteSpace([string]$definition.profession_two) -or
            [string]$definition.profession_one -eq [string]$definition.profession_two) {
            throw "Race seed $($definition.race_name) has an invalid profession pair."
        }
        if ([int]$definition.guid -le 0 -or [int]$definition.race_id -le 0 -or [int]$definition.class_id -le 0) {
            throw "Race seed $($definition.race_name) has an invalid identity."
        }
    }
    return $true
}

function ConvertTo-OracleFailureCode {
    param(
        [Parameter(Mandatory = $true)][string]$Domain,
        [Parameter(Mandatory = $true)][string]$Code
    )

    $value = ('{0}:{1}' -f $Domain, $Code).ToLowerInvariant()
    $value = [regex]::Replace($value, '[^a-z0-9_.:-]+', '_')
    $value = $value.Trim('_', ':', '.', '-')
    if ([string]::IsNullOrWhiteSpace($value)) { return 'unknown:unknown' }
    return $value
}

function Get-OracleFailureKey {
    param([Parameter(Mandatory = $true)][object]$Failure)
    return ('{0}|{1}|{2}' -f [uint32]$Failure.guid, [string]$Failure.domain, [string]$Failure.code)
}

Assert-OracleRaceSeedDefinitions | Out-Null
