# AutoWoW Autopilot V1.2 - GOAL DEPENDENCY PLANNER (pure planning structures).
# Turns player goals (schema autowow.autopilot.goal.v1) into a deterministic
# DAG of job-plan nodes (schema autowow.autopilot.goalplan.v1).
#
# Scope discipline:
#   - No execution, no sockets, no state mutation, no receipts. Plans only.
#   - Every node ANNOTATES the capability requirements of its job kind, read
#     verbatim from $script:AutopilotKindRequiredCapabilities (AutopilotLib.ps1).
#     Deployment/availability is NEVER claimed here; a node whose capabilities
#     are undeployed stays a plan node and is marked blocked-by-capability at
#     plan time by the CALLER.
#   - Unknown facts stay $null with a recorded reason (never guessed, never
#     defaulted to a plausible value).
#   - NEVER generated: grinding nodes, teleport travel, item/reward grants,
#     or any legacy-bot fallback node.
#
# This file must be dot-sourced into the same scope AFTER AutopilotLib.ps1.

Set-StrictMode -Version Latest

if (-not (Get-Command -Name 'ConvertTo-AutopilotCanonicalJson' -ErrorAction SilentlyContinue) -or
    -not (Get-Variable -Name 'AutopilotKindRequiredCapabilities' -Scope Script -ErrorAction SilentlyContinue)) {
    throw 'AutopilotGoals.ps1 must be dot-sourced into the same scope AFTER AutopilotLib.ps1 (it reads the lib helpers and the kind capability table).'
}

$script:AutopilotGoalSchema = 'autowow.autopilot.goal.v1'
$script:AutopilotGoalSchemaVersion = 1
$script:AutopilotGoalPlanSchema = 'autowow.autopilot.goalplan.v1'
$script:AutopilotGoalPlanSchemaVersion = 1

$script:AutopilotGoalKinds = @(
    'ReachLevel', 'LearnProfession', 'Stockpile', 'CraftItem',
    'RunDungeon', 'RunRaid', 'RunBattleground'
)

# Canonical 5-role party composition (matches the lib's Dungeon spec rule that
# roles must list exactly 5 roles).
$script:AutopilotGoalDungeonRoles = @('tank', 'healer', 'dps', 'dps', 'dps')

# ---------------------------------------------------------------------------
# Small helpers (goal-planner private; Autopilot-prefixed to avoid collisions)
# ---------------------------------------------------------------------------

function Get-AutopilotGoalInt64OrNull {
    # Strict integer read: returns [int64] only when the value is losslessly an
    # integer; $null otherwise (never coerced from booleans or fractional values).
    param([AllowNull()]$Value)
    if ($null -eq $Value -or $Value -is [bool]) { return $null }
    if ($Value -is [string]) {
        if ($Value -notmatch '^-?\d+$') { return $null }
        try { return [int64]$Value } catch { return $null }
    }
    if ($Value -is [System.ValueType]) {
        try {
            $asLong = [int64]$Value
            if (($Value -is [double] -or $Value -is [single] -or $Value -is [decimal]) -and
                ([double]$Value -ne [double]$asLong)) { return $null }
            return $asLong
        }
        catch { return $null }
    }
    return $null
}

function Get-AutopilotGoalIntFieldErrors {
    # Validates one integer field of a goal document. Returns an array of error
    # strings (possibly empty).
    param(
        [Parameter(Mandatory)]$Goal,
        [Parameter(Mandatory)][string]$Field,
        [Parameter(Mandatory)][int64]$Min,
        [Parameter(Mandatory)][int64]$Max,
        [switch]$Required
    )
    $fieldErrors = @()
    if (-not $Goal.Contains($Field) -or $null -eq $Goal[$Field]) {
        if ($Required) { $fieldErrors += "missing required field '$Field'" }
        return , @($fieldErrors)
    }
    $value = Get-AutopilotGoalInt64OrNull $Goal[$Field]
    if ($null -eq $value) {
        $fieldErrors += "field '$Field' must be an integer"
    }
    elseif ($value -lt $Min -or $value -gt $Max) {
        $fieldErrors += "field '$Field' value $value is out of range $Min..$Max"
    }
    return , @($fieldErrors)
}

function New-AutopilotGoalNode {
    # Builds one plan node. nodeId is deterministic and idempotent:
    # 'gn-' + first 10 hex chars of SHA-256 over
    # "<guid>|<goalIndex>|<jobKind>|<ConvertTo-AutopilotCanonicalJson spec>".
    param(
        [Parameter(Mandatory)][int64]$CharacterGuid,
        [Parameter(Mandatory)][int]$GoalIndex,
        [Parameter(Mandatory)][string]$GoalKind,
        [Parameter(Mandatory)][string]$JobKind,
        [Parameter(Mandatory)]$Spec,
        [AllowEmptyCollection()][array]$Prerequisites = @(),
        [bool]$Optional = $false,
        $Notes = $null
    )
    if (-not $script:AutopilotKindRequiredCapabilities.ContainsKey($JobKind)) {
        throw ([System.ArgumentException]::new("Unknown job kind '$JobKind': no entry in the lib capability requirement table."))
    }
    $specJson = ConvertTo-AutopilotCanonicalJson $Spec
    $hash = Get-AutopilotSha256 -Text ('{0}|{1}|{2}|{3}' -f $CharacterGuid, $GoalIndex, $JobKind, $specJson)
    [ordered]@{
        nodeId               = 'gn-' + $hash.Substring(0, 10)
        goalIndex            = [int]$GoalIndex
        goalKind             = $GoalKind
        jobKind              = $JobKind
        spec                 = $Spec
        prerequisites        = @($Prerequisites)
        # Read verbatim from AutopilotLib.ps1; annotation only - availability
        # and deployment are judged by the caller, never here.
        requiredCapabilities = @($script:AutopilotKindRequiredCapabilities[$JobKind])
        optional             = [bool]$Optional
        notes                = $Notes
    }
}

# ---------------------------------------------------------------------------
# Goal validation (fail closed)
# ---------------------------------------------------------------------------

function Test-AutopilotGoal {
    <#
    Validates a single goal document (schema autowow.autopilot.goal.v1).
    Fails closed with typed reasons on: null/malformed input, malformed JSON
    (string input), unknown schema, unsupported schema_version, unknown kind,
    and out-of-range params. Returns {Valid, Errors[]}.
    #>
    param([Parameter(Mandatory)][AllowNull()][AllowEmptyString()]$Goal)
    $errors = [System.Collections.Generic.List[string]]::new()
    # NOTE: PowerShell variables are case-insensitive, so the working copy must
    # NOT be named $goal (that would overwrite the $Goal parameter).
    $goalTable = $null

    if ($null -eq $Goal) {
        $errors.Add('goal is null (fail closed)')
    }
    elseif ($Goal -is [string]) {
        try { $goalTable = ConvertTo-AutopilotHashtable ($Goal | ConvertFrom-Json -ErrorAction Stop) }
        catch { $errors.Add("goal JSON malformed (fail closed): $($_.Exception.Message)") }
    }
    else {
        $goalTable = ConvertTo-AutopilotHashtable $Goal
    }
    if ($errors.Count -eq 0 -and -not ($goalTable -is [System.Collections.IDictionary])) {
        $errors.Add('goal is not an object (fail closed)')
        $goalTable = $null
    }

    if ($goalTable -is [System.Collections.IDictionary]) {
        if (-not $goalTable.Contains('schema') -or [string]$goalTable['schema'] -ne $script:AutopilotGoalSchema) {
            $found = if ($goalTable.Contains('schema')) { [string]$goalTable['schema'] } else { '<missing>' }
            $errors.Add("unknown goal schema '$found' (fail closed): expected $($script:AutopilotGoalSchema)")
        }
        $version = if ($goalTable.Contains('schema_version')) { Get-AutopilotGoalInt64OrNull $goalTable['schema_version'] } else { $null }
        if ($version -ne $script:AutopilotGoalSchemaVersion) {
            $foundVersion = if ($goalTable.Contains('schema_version')) { [string]$goalTable['schema_version'] } else { '<missing>' }
            $errors.Add("unsupported goal schema_version '$foundVersion' (fail closed): expected $($script:AutopilotGoalSchemaVersion)")
        }

        if (-not $goalTable.Contains('kind') -or [string]::IsNullOrWhiteSpace([string]$goalTable['kind'])) {
            $errors.Add("missing required field 'kind' (fail closed)")
        }
        elseif ([string]$goalTable['kind'] -notin $script:AutopilotGoalKinds) {
            $errors.Add("unknown goal kind '$([string]$goalTable['kind'])' (fail closed)")
        }
        else {
            switch ([string]$goalTable['kind']) {
                'ReachLevel' {
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'level' -Min 2 -Max 80 -Required)) { $errors.Add("ReachLevel: $e") }
                }
                'LearnProfession' {
                    if (-not $goalTable.Contains('profession') -or $null -eq $goalTable['profession'] -or
                        [string]::IsNullOrWhiteSpace([string]$goalTable['profession'])) {
                        $errors.Add("LearnProfession: missing required field 'profession'")
                    }
                    elseif (([string]$goalTable['profession']) -match '^\d+$') {
                        $professionId = Get-AutopilotGoalInt64OrNull ([string]$goalTable['profession'])
                        if ($null -eq $professionId -or $professionId -lt 1) {
                            $errors.Add("LearnProfession: numeric profession skill id must be a positive integer")
                        }
                    }
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'targetSkill' -Min 1 -Max 450)) { $errors.Add("LearnProfession: $e") }
                }
                'Stockpile' {
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'itemId' -Min 1 -Max ([int32]::MaxValue) -Required)) { $errors.Add("Stockpile: $e") }
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'count' -Min 1 -Max ([int32]::MaxValue) -Required)) { $errors.Add("Stockpile: $e") }
                }
                'CraftItem' {
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'itemId' -Min 1 -Max ([int32]::MaxValue) -Required)) { $errors.Add("CraftItem: $e") }
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'count' -Min 1 -Max ([int32]::MaxValue) -Required)) { $errors.Add("CraftItem: $e") }
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'recipeSpellId' -Min 1 -Max ([int32]::MaxValue))) { $errors.Add("CraftItem: $e") }
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'professionSkillId' -Min 1 -Max ([int32]::MaxValue))) { $errors.Add("CraftItem: $e") }
                }
                'RunDungeon' {
                    if (-not $goalTable.Contains('name') -or [string]::IsNullOrWhiteSpace([string]$goalTable['name'])) {
                        $errors.Add("RunDungeon: missing required field 'name'")
                    }
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'minLevel' -Min 1 -Max 80)) { $errors.Add("RunDungeon: $e") }
                }
                'RunRaid' {
                    if (-not $goalTable.Contains('name') -or [string]::IsNullOrWhiteSpace([string]$goalTable['name'])) {
                        $errors.Add("RunRaid: missing required field 'name'")
                    }
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'rosterSize' -Min 1 -Max 40)) { $errors.Add("RunRaid: $e") }
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'minLevel' -Min 1 -Max 80)) { $errors.Add("RunRaid: $e") }
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'minItemLevel' -Min 1 -Max ([int32]::MaxValue))) { $errors.Add("RunRaid: $e") }
                }
                'RunBattleground' {
                    if (-not $goalTable.Contains('name') -or [string]$goalTable['name'] -ne 'wsg') {
                        $found = if ($goalTable.Contains('name')) { [string]$goalTable['name'] } else { '<missing>' }
                        $errors.Add("RunBattleground: name must be 'wsg' in v1 (fail closed), got '$found'")
                    }
                    foreach ($e in (Get-AutopilotGoalIntFieldErrors -Goal $goalTable -Field 'teamSize' -Min 1 -Max 10)) { $errors.Add("RunBattleground: $e") }
                }
            }
        }
    }

    return [pscustomobject]@{
        Valid  = ($errors.Count -eq 0)
        Errors = @($errors)
    }
}

# ---------------------------------------------------------------------------
# Goal expansion (one goal -> nodes + edges)
# ---------------------------------------------------------------------------

function Expand-AutopilotGoal {
    <#
    Expands one validated goal into plan nodes and dependency edges.
    Deterministic: identical (goal, goalIndex, characterGuid, profile) input
    always yields byte-identical nodes and edges. Returns {nodes, edges}.
    #>
    param(
        [Parameter(Mandatory)]$Goal,
        [Parameter(Mandatory)][ValidateRange(0, [int]::MaxValue)][int]$GoalIndex,
        [Parameter(Mandatory)][ValidateRange(1, [int64]::MaxValue)][int64]$CharacterGuid,
        $Profile
    )
    $validation = Test-AutopilotGoal -Goal $Goal
    if (-not $validation.Valid) {
        throw ([System.ArgumentException]::new("Goal rejected: $($validation.Errors -join '; ')"))
    }
    # Working copy under a distinct name ($goal would alias the $Goal parameter:
    # PowerShell variable names are case-insensitive).
    $goalTable = if ($Goal -is [string]) { ConvertTo-AutopilotHashtable ($Goal | ConvertFrom-Json) } else { ConvertTo-AutopilotHashtable $Goal }
    $kind = [string]$goalTable['kind']

    $nodes = [System.Collections.Generic.List[object]]::new()
    $edges = [System.Collections.Generic.List[object]]::new()

    $nodeArgs = @{ CharacterGuid = $CharacterGuid; GoalIndex = $GoalIndex; GoalKind = $kind }

    switch ($kind) {
        'ReachLevel' {
            $quest = New-AutopilotGoalNode @nodeArgs -JobKind 'QuestLevel' -Spec ([ordered]@{
                    partyLeaderGuid = [int64]$CharacterGuid
                    targetLevel     = [int](Get-AutopilotGoalInt64OrNull $goalTable['level'])
                })
            $nodes.Add($quest)

            # Optional XP supplement, ONLY when a provided profile explicitly
            # allows the Dungeon job kind. The dungeon name is a null
            # placeholder: no concrete dungeon is ever guessed here.
            $allowsOptionalDungeon = $false
            if ($PSBoundParameters.ContainsKey('Profile') -and $null -ne $Profile) {
                $profileTable = ConvertTo-AutopilotHashtable $Profile
                if ($profileTable -is [System.Collections.IDictionary] -and $profileTable.Contains('allowedJobKinds')) {
                    $allowsOptionalDungeon = ('Dungeon' -in @($profileTable['allowedJobKinds']))
                }
            }
            if ($allowsOptionalDungeon) {
                $dungeon = New-AutopilotGoalNode @nodeArgs -JobKind 'Dungeon' -Spec ([ordered]@{
                        name  = $null
                        roles = @($script:AutopilotGoalDungeonRoles)
                    }) -Optional $true -Notes 'optional XP supplement; requires a concrete dungeon choice'
                $nodes.Add($dungeon)
                $edges.Add([ordered]@{ from = [string]$quest.nodeId; to = [string]$dungeon.nodeId })
            }
        }
        'LearnProfession' {
            # professionSkillId stays $null when only a name is known; the name
            # is recorded and the unknown is stated with its reason - never
            # resolved by guessing.
            $rawProfession = $goalTable['profession']
            $professionSkillId = $null
            $professionName = $null
            $notes = $null
            if (([string]$rawProfession) -match '^\d+$') {
                $professionSkillId = Get-AutopilotGoalInt64OrNull ([string]$rawProfession)
            }
            else {
                $professionName = [string]$rawProfession
                $notes = "professionSkillId unknown: only the profession name '$professionName' is known; id resolution needs roster/DBC facts"
            }
            $targetSkill = [int64]75
            if ($goalTable.Contains('targetSkill') -and $null -ne $goalTable['targetSkill']) {
                $targetSkill = Get-AutopilotGoalInt64OrNull $goalTable['targetSkill']
            }
            $train = New-AutopilotGoalNode @nodeArgs -JobKind 'TrainProfession' -Spec ([ordered]@{
                    professionSkillId = $professionSkillId
                    professionName    = $professionName
                    targetSkill       = [int]$targetSkill
                }) -Notes $notes
            $nodes.Add($train)
        }
        'Stockpile' {
            $itemId = Get-AutopilotGoalInt64OrNull $goalTable['itemId']
            $count = Get-AutopilotGoalInt64OrNull $goalTable['count']
            $gather = New-AutopilotGoalNode @nodeArgs -JobKind 'Gather' -Spec ([ordered]@{
                    item        = [ordered]@{ itemId = $itemId; count = $count }
                    stopAtCount = $count
                }) -Notes 'buy alternative permitted only when profile economy.allowSpending and a Restock executor exists'
            $nodes.Add($gather)
        }
        'CraftItem' {
            $itemId = Get-AutopilotGoalInt64OrNull $goalTable['itemId']
            $count = Get-AutopilotGoalInt64OrNull $goalTable['count']
            $recipeSpellId = $null
            if ($goalTable.Contains('recipeSpellId') -and $null -ne $goalTable['recipeSpellId']) {
                $recipeSpellId = Get-AutopilotGoalInt64OrNull $goalTable['recipeSpellId']
            }
            $professionSkillId = $null
            if ($goalTable.Contains('professionSkillId') -and $null -ne $goalTable['professionSkillId']) {
                $professionSkillId = Get-AutopilotGoalInt64OrNull $goalTable['professionSkillId']
            }

            # Materials are unknown without recipe facts: placeholder spec,
            # optional node, unknown stated with its reason.
            $gather = New-AutopilotGoalNode @nodeArgs -JobKind 'Gather' -Spec ([ordered]@{
                    item        = [ordered]@{ itemId = [int64]0 }
                    stopAtCount = [int64]0
                    note        = 'material list requires recipe facts; unresolved'
                }) -Optional $true -Notes 'material list requires recipe facts; unresolved'
            # Conditional training: needed only when the profession/skill is
            # unknown or below the recipe requirement. Skill facts are unknown
            # at plan time, so the node is kept but marked optional.
            $train = New-AutopilotGoalNode @nodeArgs -JobKind 'TrainProfession' -Spec ([ordered]@{
                    professionSkillId = $professionSkillId
                    professionName    = $null
                    targetSkill       = 75
                }) -Optional $true -Notes 'conditional: only needed when the profession/skill is unknown or below the recipe requirement; skill facts are unknown at plan time'
            $craft = New-AutopilotGoalNode @nodeArgs -JobKind 'Craft' -Spec ([ordered]@{
                    recipeSpellId = $recipeSpellId
                    resultItemId  = $itemId
                    count         = $count
                })
            $nodes.Add($gather)
            $nodes.Add($train)
            $nodes.Add($craft)
            $edges.Add([ordered]@{ from = [string]$gather.nodeId; to = [string]$craft.nodeId })
            $edges.Add([ordered]@{ from = [string]$train.nodeId; to = [string]$craft.nodeId })
        }
        'RunDungeon' {
            $name = [string]$goalTable['name']
            $minLevel = [int64]15
            if ($goalTable.Contains('minLevel') -and $null -ne $goalTable['minLevel']) {
                $minLevel = Get-AutopilotGoalInt64OrNull $goalTable['minLevel']
            }
            $roles = @($script:AutopilotGoalDungeonRoles)
            $repair = New-AutopilotGoalNode @nodeArgs -JobKind 'VendorRepair' -Spec ([ordered]@{})
            $restock = New-AutopilotGoalNode @nodeArgs -JobKind 'Restock' -Spec ([ordered]@{})
            $party = New-AutopilotGoalNode @nodeArgs -JobKind 'FormParty' -Spec ([ordered]@{ roles = $roles })
            $travel = New-AutopilotGoalNode @nodeArgs -JobKind 'Travel' -Spec ([ordered]@{ destination = ($name + ' entrance') })
            $dungeon = New-AutopilotGoalNode @nodeArgs -JobKind 'Dungeon' -Spec ([ordered]@{
                    name  = $name
                    roles = $roles
                }) -Prerequisites @([ordered]@{ kind = 'min-level'; params = [ordered]@{ level = $minLevel } })
            $chain = @($repair, $restock, $party, $travel, $dungeon)
            foreach ($node in $chain) { $nodes.Add($node) }
            for ($i = 0; $i -lt $chain.Count - 1; $i++) {
                $edges.Add([ordered]@{ from = [string]$chain[$i].nodeId; to = [string]$chain[$i + 1].nodeId })
            }
        }
        'RunRaid' {
            $name = [string]$goalTable['name']
            $rosterSize = [int64]10
            if ($goalTable.Contains('rosterSize') -and $null -ne $goalTable['rosterSize']) {
                $rosterSize = Get-AutopilotGoalInt64OrNull $goalTable['rosterSize']
            }
            $minLevel = [int64]80
            if ($goalTable.Contains('minLevel') -and $null -ne $goalTable['minLevel']) {
                $minLevel = Get-AutopilotGoalInt64OrNull $goalTable['minLevel']
            }
            $minItemLevel = $null
            if ($goalTable.Contains('minItemLevel') -and $null -ne $goalTable['minItemLevel']) {
                $minItemLevel = Get-AutopilotGoalInt64OrNull $goalTable['minItemLevel']
            }
            $repair = New-AutopilotGoalNode @nodeArgs -JobKind 'VendorRepair' -Spec ([ordered]@{})
            $restock = New-AutopilotGoalNode @nodeArgs -JobKind 'Restock' -Spec ([ordered]@{})
            $party = New-AutopilotGoalNode @nodeArgs -JobKind 'FormParty' -Spec ([ordered]@{ roles = @('flex') }) `
                -Notes 'raid roster: flexible role composition; concrete assignments require roster facts'
            $travel = New-AutopilotGoalNode @nodeArgs -JobKind 'Travel' -Spec ([ordered]@{ destination = ($name + ' entrance') })
            # Gear floor travels in spec.minItemLevel for the planner; the only
            # prerequisite kind used is the job-schema 'min-level'.
            $raid = New-AutopilotGoalNode @nodeArgs -JobKind 'Raid' -Spec ([ordered]@{
                    name         = $name
                    rosterSize   = $rosterSize
                    difficulty   = [int64]0
                    minItemLevel = $minItemLevel
                }) -Prerequisites @([ordered]@{ kind = 'min-level'; params = [ordered]@{ level = $minLevel } })
            $chain = @($repair, $restock, $party, $travel, $raid)
            foreach ($node in $chain) { $nodes.Add($node) }
            for ($i = 0; $i -lt $chain.Count - 1; $i++) {
                $edges.Add([ordered]@{ from = [string]$chain[$i].nodeId; to = [string]$chain[$i + 1].nodeId })
            }
        }
        'RunBattleground' {
            $name = [string]$goalTable['name']
            $teamSize = [int64]10
            if ($goalTable.Contains('teamSize') -and $null -ne $goalTable['teamSize']) {
                $teamSize = Get-AutopilotGoalInt64OrNull $goalTable['teamSize']
            }
            $party = New-AutopilotGoalNode @nodeArgs -JobKind 'FormParty' -Spec ([ordered]@{
                    roles = @(1..$teamSize | ForEach-Object { 'dps' })
                }) -Notes 'battleground bracket team; placeholder dps roles pending roster facts'
            $battleground = New-AutopilotGoalNode @nodeArgs -JobKind 'Battleground' -Spec ([ordered]@{
                    name     = $name
                    teamSize = $teamSize
                }) -Prerequisites @([ordered]@{ kind = 'min-level'; params = [ordered]@{ level = [int64]10 } })
            $nodes.Add($party)
            $nodes.Add($battleground)
            $edges.Add([ordered]@{ from = [string]$party.nodeId; to = [string]$battleground.nodeId })
        }
    }

    return [pscustomobject]@{
        nodes = @($nodes.ToArray())
        edges = @($edges.ToArray())
    }
}

# ---------------------------------------------------------------------------
# Deterministic topological order (Kahn; ready set in ascending nodeId order)
# ---------------------------------------------------------------------------

function Get-AutopilotGoalPlanOrder {
    <#
    Kahn's algorithm with a deterministic tie-break: the ready set is always
    processed in ascending (ordinal) nodeId order, so the same DAG always
    yields the same order. Throws a typed error containing 'cycle detected'
    and the unresolved nodeIds when the edges do not form a DAG.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][array]$Nodes,
        [Parameter(Mandatory)][AllowEmptyCollection()][array]$Edges
    )
    $inDegree = @{}
    $adjacency = @{}
    foreach ($node in $Nodes) {
        $table = ConvertTo-AutopilotHashtable $node
        if (-not ($table -is [System.Collections.IDictionary]) -or -not $table.Contains('nodeId') -or
            [string]::IsNullOrWhiteSpace([string]$table['nodeId'])) {
            throw ([System.ArgumentException]::new('Every node must carry a non-empty nodeId.'))
        }
        $nodeId = [string]$table['nodeId']
        if (-not $inDegree.ContainsKey($nodeId)) {
            $inDegree[$nodeId] = 0
            $adjacency[$nodeId] = [System.Collections.Generic.List[string]]::new()
        }
    }

    $seenEdges = @{}
    foreach ($edge in $Edges) {
        $table = ConvertTo-AutopilotHashtable $edge
        if (-not ($table -is [System.Collections.IDictionary]) -or -not $table.Contains('from') -or -not $table.Contains('to')) {
            throw ([System.ArgumentException]::new("Every edge must carry 'from' and 'to' nodeIds."))
        }
        $from = [string]$table['from']
        $to = [string]$table['to']
        if (-not $inDegree.ContainsKey($from)) { throw ([System.ArgumentException]::new("Edge references unknown node '$from'.")) }
        if (-not $inDegree.ContainsKey($to)) { throw ([System.ArgumentException]::new("Edge references unknown node '$to'.")) }
        $key = '{0}|{1}' -f $from, $to
        if ($seenEdges.ContainsKey($key)) { continue }
        $seenEdges[$key] = $true
        $adjacency[$from].Add($to)
        $inDegree[$to] = [int]$inDegree[$to] + 1
    }

    $ready = [System.Collections.Generic.List[string]]::new()
    foreach ($nodeId in $inDegree.Keys) {
        if ([int]$inDegree[$nodeId] -eq 0) { $ready.Add($nodeId) }
    }
    $order = [System.Collections.Generic.List[string]]::new()
    while ($ready.Count -gt 0) {
        $ready.Sort([System.StringComparer]::Ordinal)   # ascending nodeId, deterministic
        $current = $ready[0]
        $ready.RemoveAt(0)
        $order.Add($current)
        foreach ($next in $adjacency[$current]) {
            $inDegree[$next] = [int]$inDegree[$next] - 1
            if ([int]$inDegree[$next] -eq 0) { $ready.Add($next) }
        }
    }

    if ($order.Count -lt $inDegree.Keys.Count) {
        $unresolved = @($inDegree.Keys | Where-Object { [int]$inDegree[$_] -gt 0 } | Sort-Object)
        throw ([System.InvalidOperationException]::new(
                "Goal plan cycle detected: topological order is impossible; unresolved nodes (including the cycle): $($unresolved -join ', ')"))
    }
    return , @($order.ToArray())
}

# ---------------------------------------------------------------------------
# Whole-plan builder
# ---------------------------------------------------------------------------

function New-AutopilotGoalPlan {
    <#
    Validates every goal first (throwing with ALL errors), expands each goal,
    merges duplicate nodeIds idempotently (identical duplicate goals collapse
    to the first occurrence's goalIndex, so the same node generated twice is
    one node), and returns the ordered goal-plan document
    (schema autowow.autopilot.goalplan.v1) with a deterministic topological
    order. Pure planning: nothing is executed or persisted.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][array]$Goals,
        [Parameter(Mandatory)][ValidateRange(1, [int64]::MaxValue)][int64]$CharacterGuid,
        $Profile
    )
    $goalList = @($Goals)

    # 1) Validate every goal; report every error at once.
    $allErrors = [System.Collections.Generic.List[string]]::new()
    for ($i = 0; $i -lt $goalList.Count; $i++) {
        $validation = Test-AutopilotGoal -Goal $goalList[$i]
        foreach ($goalError in $validation.Errors) { $allErrors.Add("goal[$i]: $goalError") }
    }
    if ($allErrors.Count -gt 0) {
        throw ([System.ArgumentException]::new("Goal plan rejected: $($allErrors -join '; ')"))
    }

    # 2) Expand. Identical duplicate goals map to the first occurrence's
    #    goalIndex so their nodes get identical nodeIds and merge to one.
    $firstIndexByCanonicalGoal = @{}
    $nodesById = [ordered]@{}
    $edgesByKey = [ordered]@{}
    for ($i = 0; $i -lt $goalList.Count; $i++) {
        $canonicalGoal = ConvertTo-AutopilotCanonicalJson (ConvertTo-AutopilotHashtable $goalList[$i])
        $effectiveIndex = $i
        if ($firstIndexByCanonicalGoal.ContainsKey($canonicalGoal)) {
            $effectiveIndex = [int]$firstIndexByCanonicalGoal[$canonicalGoal]
        }
        else {
            $firstIndexByCanonicalGoal[$canonicalGoal] = $i
        }

        $expandArgs = @{ Goal = $goalList[$i]; GoalIndex = $effectiveIndex; CharacterGuid = $CharacterGuid }
        if ($PSBoundParameters.ContainsKey('Profile')) { $expandArgs['Profile'] = $Profile }
        $expansion = Expand-AutopilotGoal @expandArgs

        foreach ($node in @($expansion.nodes)) {
            $nodeId = [string]$node.nodeId
            if ($nodesById.Contains($nodeId)) {
                # Idempotent merge: identical regeneration is one node. Same id
                # with DIFFERENT content is a truncated-hash collision - refuse
                # rather than silently merging distinct plan steps.
                $existingJson = ConvertTo-AutopilotCanonicalJson $nodesById[$nodeId]
                $incomingJson = ConvertTo-AutopilotCanonicalJson $node
                if ($existingJson -ne $incomingJson) {
                    throw ([System.InvalidOperationException]::new("Goal plan nodeId collision: '$nodeId' was generated twice with differing content; refusing to merge."))
                }
            }
            else {
                $nodesById[$nodeId] = $node
            }
        }
        foreach ($edge in @($expansion.edges)) {
            $key = '{0}|{1}' -f [string]$edge.from, [string]$edge.to
            if (-not $edgesByKey.Contains($key)) { $edgesByKey[$key] = $edge }
        }
    }

    $nodes = @($nodesById.Values)
    $edges = @($edgesByKey.Values)
    # No @(...) wrap here: the order function already returns an array via the
    # unary-comma pattern; wrapping again would nest it one level deep.
    $order = Get-AutopilotGoalPlanOrder -Nodes $nodes -Edges $edges

    return [ordered]@{
        schema         = $script:AutopilotGoalPlanSchema
        schema_version = $script:AutopilotGoalPlanSchemaVersion
        characterGuid  = [int64]$CharacterGuid
        nodes          = $nodes
        edges          = $edges
        order          = $order
    }
}
