Set-StrictMode -Version Latest

BeforeAll {
    $script:ControlPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'autowow-control.ps1'
}

Describe 'AutoWow exact engage wrapper' {
    It 'emits the fail-closed creature-entry selector' {
        (& $script:ControlPath -Action engage -BotGuid 2 -TargetEntry 23954 -EmitRequestOnly) |
            Should -Be 'engage 2 entry 23954'
    }

    It 'emits the fail-closed exact-player selector' {
        (& $script:ControlPath -Action engage -BotGuid 2 -TargetPlayerGuid 77 -EmitRequestOnly) |
            Should -Be 'engage 2 player 77'
    }

    It 'preserves the legacy broad engage shape' {
        (& $script:ControlPath -Action engage -BotGuid 2 -EmitRequestOnly) |
            Should -Be 'engage 2'
    }

    It 'rejects TargetEntry on unrelated actions' {
        { & $script:ControlPath -Action snapshot -BotGuid 2 -TargetEntry 23954 -EmitRequestOnly } |
            Should -Throw '*TargetEntry is valid only with the engage action*'
    }

    It 'rejects TargetPlayerGuid on unrelated actions and mixed exact selectors' {
        { & $script:ControlPath -Action snapshot -BotGuid 2 -TargetPlayerGuid 77 -EmitRequestOnly } |
            Should -Throw '*TargetPlayerGuid is valid only with the engage action*'
        { & $script:ControlPath -Action engage -BotGuid 2 -TargetEntry 23954 -TargetPlayerGuid 77 -EmitRequestOnly } |
            Should -Throw '*mutually exclusive*'
    }
}
