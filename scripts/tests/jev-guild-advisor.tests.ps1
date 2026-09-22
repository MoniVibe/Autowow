BeforeAll {
    $script:AdvisorPath = Join-Path $PSScriptRoot '..\jev-guild-advisor.ps1'
    $script:Input = [ordered]@{
        state = [ordered]@{
            faction = 'Alliance'
            guilds = @(
                [ordered]@{ guildId = 101; copper = 12000; inventory = [ordered]@{ '2589' = 40 } },
                [ordered]@{ guildId = 202; copper = 8000; inventory = [ordered]@{ '2840' = 12 } }
            )
            activeContracts = @([ordered]@{ contractId = 'cloth-1'; state = 'reserve' })
            bottlenecks = @('linen supply')
        }
        allowlist = @(
            [ordered]@{ id = 'fulfill-cloth'; description = 'Fulfill the reserved cloth contract'; baselineRank = 2 },
            [ordered]@{ id = 'gather-linen'; description = 'Increase linen inventory'; baselineRank = 1 },
            [ordered]@{ id = 'hold'; description = 'Hold resources for the next milestone'; baselineRank = 3 }
        )
    } | ConvertTo-Json -Depth 12 -Compress
}

Describe 'jev-guild-advisor.ps1' {
    BeforeEach {
        [Environment]::SetEnvironmentVariable('JEV_API_KEY', $null, 'Process')
    }

    AfterAll {
        [Environment]::SetEnvironmentVariable('JEV_API_KEY', $null, 'Process')
    }

    It 'returns a deterministic baseline without a network call' {
        Mock Invoke-RestMethod { throw 'Network must not be called.' }

        $first = (& $script:AdvisorPath -InputJson $script:Input) | ConvertFrom-Json
        $second = (& $script:AdvisorPath -InputJson $script:Input) | ConvertFrom-Json

        $first.mode | Should -Be 'baseline'
        $first.selected_choice | Should -Be 'gather-linen'
        @($first.ranking) | Should -Be @('gather-linen', 'fulfill-cloth', 'hold')
        $first.advisory_only | Should -BeTrue
        ($first | ConvertTo-Json -Depth 20 -Compress) | Should -Be ($second | ConvertTo-Json -Depth 20 -Compress)
        Should -Invoke Invoke-RestMethod -Times 0 -Exactly
    }

    It 'sends one typed choice request and records validated Jev accounting' {
        [Environment]::SetEnvironmentVariable('JEV_API_KEY', 'unit-test-secret', 'Process')
        Mock Invoke-RestMethod {
            [pscustomobject]@{
                model = 'jev-1.13.0'
                answers = [pscustomobject]@{
                    strategy = [pscustomobject]@{
                        type = 'choice'; choice = 'fulfill-cloth'; confidence = 0.91
                        probabilities = [pscustomobject]@{ 'gather-linen' = 0.08; 'fulfill-cloth' = 0.90; hold = 0.02 }
                    }
                }
                usage = [pscustomobject]@{ input_tokens = 77; output_tokens = 0; cost_usd = 0.000032; credits_remaining_usd = 4.9 }
            }
        }

        $result = (& $script:AdvisorPath -InputJson $script:Input -UseJev -TimeoutSeconds 9) | ConvertFrom-Json

        $result.mode | Should -Be 'jev'
        $result.selected_choice | Should -Be 'fulfill-cloth'
        @($result.ranking) | Should -Be @('fulfill-cloth', 'gather-linen', 'hold')
        $result.model | Should -Be 'jev-1.13.0'
        $result.usage.input_tokens | Should -Be 77
        $result.cost_usd | Should -Be 0.000032
        Should -Invoke Invoke-RestMethod -Times 1 -Exactly -ParameterFilter {
            $Uri -eq 'https://jevtypesafeai.com/api/v1/decide' -and
            $Method -eq 'Post' -and $TimeoutSec -eq 9 -and
            $Headers.Authorization -eq 'Bearer unit-test-secret' -and
            ($Body | ConvertFrom-Json).questions.strategy.type -eq 'choice'
        }
    }

    It 'falls back when Jev returns a choice outside the allowlist' {
        [Environment]::SetEnvironmentVariable('JEV_API_KEY', 'unit-test-secret', 'Process')
        Mock Invoke-RestMethod {
            [pscustomobject]@{
                model = 'jev-1.13.0'
                answers = [pscustomobject]@{ strategy = [pscustomobject]@{
                    type = 'choice'; choice = 'buy-auction-house'; confidence = 0.99
                    probabilities = [pscustomobject]@{ 'gather-linen' = 0.1; 'fulfill-cloth' = 0.1; hold = 0.8 }
                } }
                usage = [pscustomobject]@{ input_tokens = 10; cost_usd = 0.00001 }
            }
        }

        $result = (& $script:AdvisorPath -InputJson $script:Input -UseJev) | ConvertFrom-Json
        $result.mode | Should -Be 'fallback'
        $result.selected_choice | Should -Be 'gather-linen'
        $result.fallback_reason | Should -Match 'outside the controller allowlist'
        Should -Invoke Invoke-RestMethod -Times 1 -Exactly
    }

    It 'falls back without a call when explicitly enabled but the process key is absent' {
        Mock Invoke-RestMethod { throw 'Network must not be called.' }
        $result = (& $script:AdvisorPath -InputJson $script:Input -UseJev) | ConvertFrom-Json
        $result.mode | Should -Be 'fallback'
        $result.selected_choice | Should -Be 'gather-linen'
        $result.fallback_reason | Should -Match 'JEV_API_KEY is not set'
        Should -Invoke Invoke-RestMethod -Times 0 -Exactly
    }

    It 'does not expose the API key when the transport fails' {
        [Environment]::SetEnvironmentVariable('JEV_API_KEY', 'unit-test-secret', 'Process')
        Mock Invoke-RestMethod { throw 'transport included unit-test-secret' }

        $json = & $script:AdvisorPath -InputJson $script:Input -UseJev
        $result = $json | ConvertFrom-Json

        $result.mode | Should -Be 'fallback'
        $result.fallback_reason | Should -Be 'Jev request failed; deterministic baseline selected.'
        $json | Should -Not -Match 'unit-test-secret'
        Should -Invoke Invoke-RestMethod -Times 1 -Exactly
    }

    It 'rejects malformed controller input before considering Jev' {
        $bad = $script:Input | ConvertFrom-Json
        $bad.allowlist = @('only-one')
        { & $script:AdvisorPath -InputJson ($bad | ConvertTo-Json -Depth 12 -Compress) } |
            Should -Throw '*2 to 4*'
    }
}
