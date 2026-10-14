[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Rom,
    [string] $NetworkingRoot = "",
    [string] $FixturePath = "",
    [switch] $ExternalRelay,
    [string] $NodeExecutable = "node",
    [string] $BuildDirectory = "build-pc-windows",
    [ValidateRange(8, 120)]
    [int] $Seconds = 30,
    [ValidateRange(1, 4)]
    [int] $Players = 4,
    [switch] $InterruptInitialBootstrap,
    [switch] $ColdJoinAfterAuthorityRestart,
    [ValidateRange(0, 3)]
    [int] $ClientReconnectCycles = 0,
    [switch] $ReconnectEverySeat,
    [ValidateRange(0, 3)]
    [int] $AllSeatReconnectCycles = 0,
    [ValidateRange(0, 1000000)]
    [int] $InjectClientDesyncTick = 0,
    [ValidateRange(2, 1000)]
    [int] $DropOutgoingEvery = 10,
    [ValidateRange(2, 1000)]
    [int] $DropIncomingEvery = 10,
    [ValidateRange(1024, 65535)]
    [int] $RelayPort = 30421
)

$ErrorActionPreference = "Stop"
if ($ReconnectEverySeat -and $AllSeatReconnectCycles -ne 0) {
    throw "ReconnectEverySeat is the one-cycle shorthand; do not combine it with AllSeatReconnectCycles."
}
$allSeatCycles = if ($ReconnectEverySeat) { 1 } else {
    $AllSeatReconnectCycles
}
if ($ClientReconnectCycles -ne 0 -and $allSeatCycles -ne 0) {
    throw "Translated-client reconnect and all-seat reconnect are separate gates."
}
if ($InjectClientDesyncTick -ne 0 -and
    ($InterruptInitialBootstrap -or $ColdJoinAfterAuthorityRestart -or
     $ClientReconnectCycles -ne 0 -or $allSeatCycles -ne 0)) {
    throw "Injected divergence is an independent recovery gate."
}
if ($InterruptInitialBootstrap -and
    ($ColdJoinAfterAuthorityRestart -or $ClientReconnectCycles -ne 0 -or
     $allSeatCycles -ne 0)) {
    throw "Initial-bootstrap interruption is a separate recovery gate."
}
if ($ColdJoinAfterAuthorityRestart -and $ClientReconnectCycles -ne 0) {
    throw "Use the all-seat mode to combine authority and client recovery."
}
if ($ColdJoinAfterAuthorityRestart -and $allSeatCycles -gt 2) {
    throw "The local fixture has three client credential rotations; authority restart consumes seat 0's first rotation, leaving at most two all-seat cycles."
}
$root = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    (Resolve-Path $BuildDirectory).Path
} else {
    (Resolve-Path (Join-Path $root $BuildDirectory)).Path
}
$romPath = (Resolve-Path $Rom).Path
$quotedRom = '"' + $romPath.Replace('"', '\"') + '"'
$game = Join-Path $build "BattleTanxGA.exe"
$bot = Join-Path $build "btga_dedicated_client_bot.exe"
$fixtureGenerator = Join-Path $PSScriptRoot "create-local-dedicated-fixture.js"
foreach ($required in @($game, $bot)) {
    if (-not (Test-Path $required -PathType Leaf)) {
        throw "Required local integration file is missing: $required"
    }
}

$evidenceRoot = Join-Path $build "local-dedicated-evidence"
New-Item $evidenceRoot -ItemType Directory -Force | Out-Null
$runId = "run-{0}-{1}-{2}" -f [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssfffZ"),
    $PID, ([Guid]::NewGuid().ToString("N").Substring(0, 8))
$evidence = Join-Path $evidenceRoot $runId
New-Item $evidence -ItemType Directory | Out-Null
Set-Content (Join-Path $evidenceRoot "latest.txt") $evidence -Encoding utf8
$fixtureFile = if ($FixturePath) {
    (Resolve-Path $FixturePath).Path
} else {
    if (-not $NetworkingRoot) {
        throw "NetworkingRoot is required when FixturePath is not supplied."
    }
    $networking = (Resolve-Path $NetworkingRoot).Path
    $relay = Join-Path $networking "server/relay/src/server.js"
    foreach ($required in @($relay, $fixtureGenerator)) {
        if (-not (Test-Path $required -PathType Leaf)) {
            throw "Required local integration file is missing: $required"
        }
    }
    $node = (Get-Command $NodeExecutable -ErrorAction Stop).Source
    $generated = Join-Path $evidence "fixture.json"
    # Credentials expire quickly and are generated immediately before admission.
    & $node $fixtureGenerator $generated $networking $RelayPort "127.0.0.1"
    if ($LASTEXITCODE -ne 0) { throw "Local credential generation failed." }
    $generated
}
$fixture = Get-Content $fixtureFile -Raw | ConvertFrom-Json

$environmentNames = @(
    "BTGA_RELAY_PUBLIC_KEY", "BTGA_RELAY_PORT",
    "BTGA_DEDICATED_SESSION", "BTGA_DEDICATED_RELAY_HOST",
    "BTGA_DEDICATED_RELAY_PORT", "BTGA_DEDICATED_TOKEN",
    "BTGA_DEDICATED_LOBBY",
    "BTGA_DEDICATED_PLAYERS", "BTGA_DEDICATED_STATE_DIR",
    "BTGA_NET_TEST_FAULTS",
    "BTGA_NET_TEST_DROP_OUTGOING_EVERY", "BTGA_NET_TEST_DROP_INCOMING_EVERY",
    "BTGA_NET_TEST_DELAY_MS", "BTGA_NET_TEST_SCRIPTED_INPUT",
    "BTGA_NET_TEST_DESYNC_TICK"
)
$previous = @{}
foreach ($name in $environmentNames) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name)
}
$processes = [System.Collections.Generic.List[Diagnostics.Process]]::new()
$peers = [System.Collections.Generic.List[Diagnostics.Process]]::new()
function Read-AuthorityHealth([string] $Path) {
    for ($attempt = 0; $attempt -lt 40; ++$attempt) {
        try {
            if (Test-Path $Path -PathType Leaf) {
                return Get-Content $Path -Raw | ConvertFrom-Json
            }
        } catch {
            # The producer replaces the small record between these operations.
        }
        Start-Sleep -Milliseconds 25
    }
    throw "Authority process health could not be read: $Path"
}
try {
    if (-not $ExternalRelay) {
        if ($FixturePath) {
            throw "FixturePath requires ExternalRelay; its signing key is not available."
        }
        $env:BTGA_RELAY_PUBLIC_KEY = [string]$fixture.relay_public_key
        $env:BTGA_RELAY_PORT = [string]$RelayPort
        $relayProcess = Start-Process $node -PassThru -WindowStyle Minimized `
            -ArgumentList @('"' + $relay + '"') `
            -RedirectStandardOutput (Join-Path $evidence "relay.stdout.log") `
            -RedirectStandardError (Join-Path $evidence "relay.stderr.log")
        $processes.Add($relayProcess)
        Start-Sleep -Milliseconds 500
        if ($relayProcess.HasExited) { throw "Local UDP relay exited during startup." }
    }

    $env:BTGA_DEDICATED_SESSION = [string]$fixture.session
    $env:BTGA_DEDICATED_RELAY_HOST = [string]$fixture.relay_host
    $env:BTGA_DEDICATED_RELAY_PORT = [string]$fixture.relay_port
    $env:BTGA_DEDICATED_PLAYERS = [string]$Players
    $stateDirectory = Join-Path $evidence "authority-state"
    New-Item $stateDirectory -ItemType Directory | Out-Null
    $env:BTGA_DEDICATED_STATE_DIR = $stateDirectory
    $env:BTGA_NET_TEST_FAULTS = "1"
    $env:BTGA_NET_TEST_DROP_OUTGOING_EVERY = [string]$DropOutgoingEvery
    $env:BTGA_NET_TEST_DROP_INCOMING_EVERY = [string]$DropIncomingEvery
    $env:BTGA_NET_TEST_DELAY_MS = $null
    $env:BTGA_NET_TEST_SCRIPTED_INPUT = $null
    $env:BTGA_NET_TEST_DESYNC_TICK = if ($InjectClientDesyncTick -ne 0) {
        [string]$InjectClientDesyncTick
    } else { $null }

    $env:BTGA_DEDICATED_TOKEN = [string]$fixture.authority_token
    $authority = Start-Process $game -PassThru -WindowStyle Minimized `
        -ArgumentList @("--dedicated-server", $quotedRom) `
        -RedirectStandardOutput (Join-Path $evidence "authority.stdout.log") `
        -RedirectStandardError (Join-Path $evidence "authority.stderr.log")
    $processes.Add($authority)
    Start-Sleep -Milliseconds 750

    $translatedSeconds = if ($InterruptInitialBootstrap) {
        $Seconds + 30
    } elseif ($allSeatCycles -ne 0) {
        $Seconds + 15 + 5 * $Players * $allSeatCycles
    } else { $Seconds }
    $env:BTGA_DEDICATED_TOKEN = [string]$fixture.client_tokens[0]
    $client = Start-Process $game -PassThru -WindowStyle Minimized `
        -ArgumentList @("--dedicated-headless-direct", [string]$translatedSeconds, $quotedRom) `
        -RedirectStandardOutput (Join-Path $evidence "client.stdout.log") `
        -RedirectStandardError (Join-Path $evidence "client.stderr.log")
    $processes.Add($client)

    # Protocol witnesses remain reliable so the deterministic fault profile
    # targets the translated authority/client recovery path rather than token
    # admission. Each witness sends a distinct input and verifies its echo.
    Start-Sleep -Milliseconds 250
    $peerSeconds = if ($InterruptInitialBootstrap) {
        $Seconds + 30
    } elseif ($allSeatCycles -ne 0) {
        $Seconds + 20 + 5 * $Players * $allSeatCycles +
            $(if ($ColdJoinAfterAuthorityRestart) { 15 } else { 0 })
    } elseif ($ColdJoinAfterAuthorityRestart) {
        # Cover the initial run, forced restart and replacement-client run,
        # then finish soon enough for the gate's bounded validation wait.
        $Seconds + 10
    } elseif ($ClientReconnectCycles -ne 0) {
        $Seconds + 15 + 5 * $ClientReconnectCycles
    } else {
        $Seconds
    }
    $peerOutputPaths = @{}
    $peerErrorPaths = @{}
    for ($seat = 1; $seat -lt $Players; ++$seat) {
        $env:BTGA_NET_TEST_FAULTS = $null
        $env:BTGA_DEDICATED_TOKEN = [string]$fixture.client_tokens[$seat]
        $peerOutputPaths[$seat] = Join-Path $evidence "bot-$seat.stdout.log"
        $peerErrorPaths[$seat] = Join-Path $evidence "bot-$seat.stderr.log"
        $peer = Start-Process $bot -PassThru -WindowStyle Minimized `
            -ArgumentList @([string]$seat, [string]$peerSeconds) `
            -RedirectStandardOutput $peerOutputPaths[$seat] `
            -RedirectStandardError $peerErrorPaths[$seat]
        $processes.Add($peer)
        $peers.Add($peer)
        $env:BTGA_NET_TEST_FAULTS = "1"
    }

    $clientErrorPath = Join-Path $evidence "client.stderr.log"
    $clientErrorPaths = [System.Collections.Generic.List[string]]::new()
    $clientErrorPaths.Add($clientErrorPath)
    $authorityErrorPaths = [System.Collections.Generic.List[string]]::new()
    $authorityErrorPaths.Add((Join-Path $evidence "authority.stderr.log"))
    if ($ColdJoinAfterAuthorityRestart -or $ClientReconnectCycles -ne 0 -or
        $allSeatCycles -ne 0) {
        # The headless translated authority advances at the host's available
        # CPU rate. Wait for the checkpoint itself rather than assuming tick
        # 120 always arrives within 20 wall-clock seconds on a loaded machine.
        $tickDeadline = [DateTime]::UtcNow.AddSeconds(120)
        do {
            Start-Sleep -Milliseconds 100
            $beforeRestart = [string](Get-Content $authorityErrorPaths[0] -Raw)
            $durableTickReached = $beforeRestart -match "BTGA_AUTHORITY tick=1[2-9][0-9]"
        } until ($durableTickReached -or $authority.HasExited -or
            [DateTime]::UtcNow -ge $tickDeadline)
        if (-not $durableTickReached) {
            throw "Authority did not reach a positive recovery checkpoint."
        }
    }

    if ($InterruptInitialBootstrap) {
        # Kill the translated endpoint while the authority is still pacing the
        # initial checkpoint. This deterministically covers process death with
        # an unfinished tick-zero generation, the transition that previously
        # survived only as implicit array state.
        $initialDeadline = [DateTime]::UtcNow.AddSeconds(60)
        do {
            Start-Sleep -Milliseconds 20
            $initialAuthorityLog =
                [string](Get-Content $authorityErrorPaths[0] -Raw)
            $initialTransferStarted = $initialAuthorityLog -match
                "BTGA_AUTHORITY all $Players seats ready"
        } until ($initialTransferStarted -or $client.HasExited -or
            [DateTime]::UtcNow -ge $initialDeadline)
        if (-not $initialTransferStarted) {
            throw "Authority did not start the initial bootstrap."
        }
        if (-not $client.HasExited) {
            Stop-Process -Id $client.Id -Force
            $client.WaitForExit(5000) | Out-Null
        }

        $positiveDeadline = [DateTime]::UtcNow.AddSeconds(60)
        do {
            Start-Sleep -Milliseconds 50
            $initialAuthorityLog =
                [string](Get-Content $authorityErrorPaths[0] -Raw)
            $positiveCheckpoint = $initialAuthorityLog -match
                "BTGA_AUTHORITY tick=[3-9][0-9]"
        } until ($positiveCheckpoint -or $authority.HasExited -or
            [DateTime]::UtcNow -ge $positiveDeadline)
        if (-not $positiveCheckpoint) {
            throw "Authority did not create a positive checkpoint after client loss."
        }

        $clientErrorPath = Join-Path $evidence "client-bootstrap-resumed.stderr.log"
        $clientErrorPaths.Add($clientErrorPath)
        $env:BTGA_DEDICATED_TOKEN =
            [string]$fixture.client_reconnect_tokens[0][0]
        $client = Start-Process $game -PassThru -WindowStyle Minimized `
            -ArgumentList @("--dedicated-headless-direct", [string]$Seconds,
                $quotedRom) `
            -RedirectStandardOutput (Join-Path $evidence `
                "client-bootstrap-resumed.stdout.log") `
            -RedirectStandardError $clientErrorPath
        $processes.Add($client)

        $resumeDeadline = [DateTime]::UtcNow.AddSeconds(60)
        do {
            Start-Sleep -Milliseconds 100
            $resumeLog = if (Test-Path $clientErrorPath) {
                [string](Get-Content $clientErrorPath -Raw)
            } else { "" }
            $resumeAuthorityLog =
                [string](Get-Content $authorityErrorPaths[0] -Raw)
            $positiveApplied = $resumeLog -match
                "BTGA_CLIENT applied snapshot tick=[1-9][0-9]*"
            $replayComplete = $resumeLog -match
                "BTGA_CLIENT journal replay complete"
            $barrierReleased = $resumeAuthorityLog -match
                "BTGA_AUTHORITY recovery complete head=[1-9][0-9]*"
        } until (($positiveApplied -and $replayComplete -and $barrierReleased) -or
            $client.HasExited -or $authority.HasExited -or
            [DateTime]::UtcNow -ge $resumeDeadline)
        if (-not $positiveApplied -or -not $replayComplete -or
            -not $barrierReleased) {
            throw "Interrupted bootstrap did not supersede, replay, and converge."
        }
    }

    if ($ColdJoinAfterAuthorityRestart) {

        foreach ($process in @($client, $authority)) {
            if (-not $process.HasExited) {
                Stop-Process -Id $process.Id -Force
                $process.WaitForExit(5000) | Out-Null
            }
        }
        Start-Sleep -Milliseconds 250

        $restartAuthorityError = Join-Path $evidence "authority-restart.stderr.log"
        $authorityErrorPaths.Add($restartAuthorityError)
        $env:BTGA_DEDICATED_TOKEN = [string]$fixture.authority_restart_token
        $authority = Start-Process $game -PassThru -WindowStyle Minimized `
            -ArgumentList @("--dedicated-server", $quotedRom) `
            -RedirectStandardOutput (Join-Path $evidence "authority-restart.stdout.log") `
            -RedirectStandardError $restartAuthorityError
        $processes.Add($authority)
        Start-Sleep -Milliseconds 750

        $clientErrorPath = Join-Path $evidence "client-restart.stderr.log"
        $clientErrorPaths.Add($clientErrorPath)
        $env:BTGA_DEDICATED_TOKEN = [string]$fixture.client_restart_tokens[0]
        $client = Start-Process $game -PassThru -WindowStyle Minimized `
            -ArgumentList @("--dedicated-headless-direct", [string]$translatedSeconds,
                $quotedRom) `
            -RedirectStandardOutput (Join-Path $evidence "client-restart.stdout.log") `
            -RedirectStandardError $clientErrorPath
        $processes.Add($client)

        if ($allSeatCycles -ne 0) {
            # Do not immediately replace the freshly bootstrapped process. The
            # combined gate first proves that the replacement authority and a
            # cold translated client converge on persisted state, then starts
            # rotating every seat on that same canonical timeline.
            $restartDeadline = [DateTime]::UtcNow.AddSeconds(60)
            do {
                Start-Sleep -Milliseconds 100
                $restartClientLog = if (Test-Path $clientErrorPath) {
                    [string](Get-Content $clientErrorPath -Raw)
                } else { "" }
                $restartAuthorityLog = if (Test-Path $restartAuthorityError) {
                    [string](Get-Content $restartAuthorityError -Raw)
                } else { "" }
                $restartApplied = $restartClientLog -match
                    "BTGA_CLIENT (applied|confirmed) snapshot tick=[1-9][0-9]*"
                $restartReleased = $restartAuthorityLog -match
                    "BTGA_AUTHORITY recovery complete head=[1-9][0-9]*"
            } until (($restartApplied -and $restartReleased) -or
                $client.HasExited -or $authority.HasExited -or
                [DateTime]::UtcNow -ge $restartDeadline)
            if (-not $restartApplied -or -not $restartReleased) {
                throw "Cold client did not converge with the replacement authority."
            }
        }
    }

    if ($ClientReconnectCycles -ne 0) {
        for ($cycle = 1; $cycle -le $ClientReconnectCycles; ++$cycle) {
            if (-not $client.HasExited) {
                Stop-Process -Id $client.Id -Force
                $client.WaitForExit(5000) | Out-Null
            }
            Start-Sleep -Milliseconds 250

            $clientErrorPath = Join-Path $evidence `
                ("client-reconnect-{0}.stderr.log" -f $cycle)
            $clientErrorPaths.Add($clientErrorPath)
            $env:BTGA_DEDICATED_TOKEN =
                [string]$fixture.client_reconnect_tokens[0][$cycle - 1]
            $client = Start-Process $game -PassThru -WindowStyle Minimized `
                -ArgumentList @("--dedicated-headless-direct", [string]$Seconds, $quotedRom) `
                -RedirectStandardOutput (Join-Path $evidence `
                    ("client-reconnect-{0}.stdout.log" -f $cycle)) `
                -RedirectStandardError $clientErrorPath
            $processes.Add($client)

            $reconnectDeadline = [DateTime]::UtcNow.AddSeconds(20)
            do {
                Start-Sleep -Milliseconds 100
                $reconnectLog = if (Test-Path $clientErrorPath) {
                    [string](Get-Content $clientErrorPath -Raw)
                } else { "" }
                $reconnected = $reconnectLog -match
                    "BTGA_CLIENT (applied|confirmed) snapshot tick=[1-9][0-9]*"
            } until ($reconnected -or $client.HasExited -or
                [DateTime]::UtcNow -ge $reconnectDeadline)
            if (-not $reconnected) {
                throw "Translated client reconnect cycle $cycle did not catch up."
            }
            $barrierDeadline = [DateTime]::UtcNow.AddSeconds(30)
            do {
                Start-Sleep -Milliseconds 100
                $liveAuthorityLog = [string](Get-Content $authorityErrorPaths[0] -Raw)
                $completedBarriers = [regex]::Matches(
                    $liveAuthorityLog,
                    "BTGA_AUTHORITY recovery complete head=[1-9][0-9]*").Count
            } until ($completedBarriers -ge $cycle -or $authority.HasExited -or
                [DateTime]::UtcNow -ge $barrierDeadline)
            if ($completedBarriers -lt $cycle) {
                throw "Authority did not release reconnect barrier $cycle."
            }
        }
    } elseif ($allSeatCycles -ne 0) {
        # Replace the translated seat first, then each protocol witness. This
        # proves that recovery is keyed by authenticated seat identity rather
        # than by a special-case client role. Repeating the sweep consumes a
        # fresh rotated credential for every seat and catches leaked replay,
        # snapshot, or barrier state between recoveries.
        $liveAuthorityErrorPath =
            $authorityErrorPaths[$authorityErrorPaths.Count - 1]
        $barriersBeforeAllSeat = [regex]::Matches(
            [string](Get-Content $liveAuthorityErrorPath -Raw),
            "BTGA_AUTHORITY recovery complete head=[1-9][0-9]*").Count
        for ($cycle = 1; $cycle -le $allSeatCycles; ++$cycle) {
            if (-not $client.HasExited) {
                Stop-Process -Id $client.Id -Force
                $client.WaitForExit(5000) | Out-Null
            }
            $clientErrorPath = Join-Path $evidence `
                ("client-reconnect-cycle-{0}-seat-0.stderr.log" -f $cycle)
            $clientErrorPaths.Add($clientErrorPath)
            $seatZeroTokenIndex = ($cycle - 1) +
                $(if ($ColdJoinAfterAuthorityRestart) { 1 } else { 0 })
            $env:BTGA_DEDICATED_TOKEN =
                [string]$fixture.client_reconnect_tokens[0][$seatZeroTokenIndex]
            $client = Start-Process $game -PassThru -WindowStyle Minimized `
                -ArgumentList @("--dedicated-headless-direct",
                    [string]$translatedSeconds, $quotedRom) `
                -RedirectStandardOutput (Join-Path $evidence `
                    ("client-reconnect-cycle-{0}-seat-0.stdout.log" -f $cycle)) `
                -RedirectStandardError $clientErrorPath
            $processes.Add($client)

            $seatZeroDeadline = [DateTime]::UtcNow.AddSeconds(60)
            do {
                Start-Sleep -Milliseconds 100
                $seatZeroLog = if (Test-Path $clientErrorPath) {
                    [string](Get-Content $clientErrorPath -Raw)
                } else { "" }
                $seatZeroReady = $seatZeroLog -match
                    "BTGA_CLIENT (applied|confirmed) snapshot tick=[1-9][0-9]*"
            } until ($seatZeroReady -or $client.HasExited -or
                [DateTime]::UtcNow -ge $seatZeroDeadline)
            if (-not $seatZeroReady) {
                throw "Translated seat 0 did not recover in cycle $cycle."
            }
            $expectedBarrier = $barriersBeforeAllSeat +
                (($cycle - 1) * $Players) + 1
            $barrierDeadline = [DateTime]::UtcNow.AddSeconds(60)
            do {
                Start-Sleep -Milliseconds 100
                $liveAuthorityLog =
                    [string](Get-Content $liveAuthorityErrorPath -Raw)
                $completedBarriers = [regex]::Matches(
                    $liveAuthorityLog,
                    "BTGA_AUTHORITY recovery complete head=[1-9][0-9]*").Count
            } until ($completedBarriers -ge $expectedBarrier -or
                $authority.HasExited -or [DateTime]::UtcNow -ge $barrierDeadline)
            if ($completedBarriers -lt $expectedBarrier) {
                throw "Authority did not release seat 0 in cycle $cycle."
            }

            for ($seat = 1; $seat -lt $Players; ++$seat) {
                $oldPeer = $peers[$seat - 1]
                if (-not $oldPeer.HasExited) {
                    Stop-Process -Id $oldPeer.Id -Force
                    $oldPeer.WaitForExit(5000) | Out-Null
                }
                $env:BTGA_NET_TEST_FAULTS = $null
                $env:BTGA_DEDICATED_TOKEN =
                    [string]$fixture.client_reconnect_tokens[$seat][$cycle - 1]
                $peerOutputPaths[$seat] = Join-Path $evidence `
                    ("bot-{0}-reconnect-cycle-{1}.stdout.log" -f $seat, $cycle)
                $peerErrorPaths[$seat] = Join-Path $evidence `
                    ("bot-{0}-reconnect-cycle-{1}.stderr.log" -f $seat, $cycle)
                $peer = Start-Process $bot -PassThru -WindowStyle Minimized `
                    -ArgumentList @([string]$seat, [string]$peerSeconds, "--resume") `
                    -RedirectStandardOutput $peerOutputPaths[$seat] `
                    -RedirectStandardError $peerErrorPaths[$seat]
                $processes.Add($peer)
                $peers[$seat - 1] = $peer
                $env:BTGA_NET_TEST_FAULTS = "1"

                $peerDeadline = [DateTime]::UtcNow.AddSeconds(60)
                do {
                    Start-Sleep -Milliseconds 100
                    $peerLog = if (Test-Path $peerOutputPaths[$seat]) {
                        [string](Get-Content $peerOutputPaths[$seat] -Raw)
                    } else { "" }
                    $peerReady = $peerLog -match "BOOTSTRAP_TICK=[1-9][0-9]*"
                } until ($peerReady -or $peer.HasExited -or
                    [DateTime]::UtcNow -ge $peerDeadline)
                if (-not $peerReady) {
                    throw "Protocol witness seat $seat did not recover in cycle $cycle."
                }
                $expectedBarrier = $barriersBeforeAllSeat +
                    (($cycle - 1) * $Players) + $seat + 1
                $barrierDeadline = [DateTime]::UtcNow.AddSeconds(60)
                do {
                    Start-Sleep -Milliseconds 100
                    $liveAuthorityLog =
                        [string](Get-Content $liveAuthorityErrorPath -Raw)
                    $completedBarriers = [regex]::Matches(
                        $liveAuthorityLog,
                        "BTGA_AUTHORITY recovery complete head=[1-9][0-9]*").Count
                } until ($completedBarriers -ge $expectedBarrier -or
                    $authority.HasExited -or
                    [DateTime]::UtcNow -ge $barrierDeadline)
                if ($completedBarriers -lt $expectedBarrier) {
                    throw "Authority did not release seat $seat in cycle $cycle."
                }
                $readyDeadline = [DateTime]::UtcNow.AddSeconds(60)
                do {
                    Start-Sleep -Milliseconds 100
                    $peerLog = if (Test-Path $peerOutputPaths[$seat]) {
                        [string](Get-Content $peerOutputPaths[$seat] -Raw)
                    } else { "" }
                    $peerObservedGameplay =
                        $peerLog -match "RECOVERY_READY tick=[1-9][0-9]*"
                } until ($peerObservedGameplay -or $peer.HasExited -or
                    [DateTime]::UtcNow -ge $readyDeadline)
                if (-not $peerObservedGameplay) {
                    throw "Protocol witness seat $seat did not resume gameplay in cycle $cycle."
                }
            }
        }
    }

    if ($allSeatCycles -ne 0) {
        # Every final replacement has already supplied a positive bootstrap,
        # contiguous-frame, and own-input witness. Keep all four alive through
        # the last sweep, then end only the processes owned by this gate rather
        # than waiting out their deliberately extended observation windows.
        Start-Sleep -Milliseconds 500
        if (-not $client.HasExited) {
            Stop-Process -Id $client.Id -Force
            $client.WaitForExit(5000) | Out-Null
        }
        foreach ($peer in $peers) {
            if (-not $peer.HasExited) {
                Stop-Process -Id $peer.Id -Force
                $peer.WaitForExit(5000) | Out-Null
            }
        }
    }

    if (-not $client.WaitForExit(($translatedSeconds + 30) * 1000)) {
        throw "Translated client did not complete its timed run."
    }
    $client.WaitForExit()
    $clientLog = [string](Get-Content $clientErrorPath -Raw)
    $requiredSnapshot = if ($InterruptInitialBootstrap -or
        $ColdJoinAfterAuthorityRestart -or
        $ClientReconnectCycles -ne 0 -or $allSeatCycles -ne 0) {
        "BTGA_CLIENT (applied|confirmed) snapshot tick=[1-9][0-9]*"
    } else {
        "BTGA_CLIENT (applied|confirmed) snapshot tick=0"
    }
    $completedTimedRun = $clientLog.Contains("Headless boot window elapsed")
    if ($clientLog -notmatch $requiredSnapshot -or
        ($allSeatCycles -eq 0 -and -not $completedTimedRun)) {
        throw "Translated client did not install the bootstrap and complete its run."
    }
    $profile = "outgoing=$DropOutgoingEvery incoming=$DropIncomingEvery"
    if (-not $clientLog.Contains("BTGA_CLIENT test UDP loss $profile")) {
        throw "Translated client did not activate the requested loss profile."
    }

    $authorityLog = [string](($authorityErrorPaths | ForEach-Object {
        Get-Content $_ -Raw
    }) -join "`n")
    if (-not $authorityLog.Contains("BTGA_AUTHORITY all $Players seats ready") -or
        -not $authorityLog.Contains("BTGA_AUTHORITY test UDP loss $profile")) {
        throw "Authority did not reach the requested lossy $Players-player session."
    }
    if ($ColdJoinAfterAuthorityRestart -and
        $authorityLog -notmatch
            "BTGA_AUTHORITY recovered checkpoint=[1-9][0-9]* journal_head=[1-9][0-9]*") {
        throw "Replacement authority did not restore persistent gameplay state."
    }
    if ($ClientReconnectCycles -ne 0 -or $allSeatCycles -ne 0 -or
        $InjectClientDesyncTick -ne 0) {
        $recoveryCount = [regex]::Matches(
            $authorityLog, "BTGA_AUTHORITY recovery complete head=[1-9][0-9]*").Count
        $expectedRecoveries = if ($InjectClientDesyncTick -ne 0) {
            1
        } elseif ($allSeatCycles -ne 0) {
            $Players * $allSeatCycles +
                $(if ($ColdJoinAfterAuthorityRestart) { 1 } else { 0 })
        } else {
            $ClientReconnectCycles
        }
        if ($recoveryCount -lt $expectedRecoveries) {
            throw "Authority did not complete every seat-recovery barrier."
        }
    }
    foreach ($path in $clientErrorPaths) {
        $log = [string](Get-Content $path -Raw)
        $desyncCount = [regex]::Matches($log, "BTGA_CLIENT DESYNC").Count
        $expectedInjectedRecovery = $InjectClientDesyncTick -ne 0 -and
            $path -eq $clientErrorPath -and
            $desyncCount -eq 1 -and
            $log.Contains("BTGA_CLIENT injected test desync tick=$InjectClientDesyncTick") -and
            $log.Contains("BTGA_CLIENT requested canonical resynchronization") -and
            -not $log.Contains("BTGA_CLIENT recovery quarantined")
        if (($desyncCount -ne 0 -and -not $expectedInjectedRecovery) -or
            $log.Contains("BTGA_CLIENT authority timeout")) {
            throw "Translated client diverged or lost the authority."
        }
    }

    # The restart supervisor relies on this record to distinguish an advancing
    # translated loop from a merely alive relay thread. Require two valid,
    # increasing samples while the authority is still running.
    $healthPath = Join-Path $stateDirectory "authority-health.json"
    if (-not (Test-Path $healthPath -PathType Leaf)) {
        throw "Authority did not publish process health."
    }
    $healthFirst = Read-AuthorityHealth $healthPath
    Start-Sleep -Milliseconds 1200
    $healthSecond = Read-AuthorityHealth $healthPath
    if ([uint32]$healthFirst.version -ne 1 -or
        [uint64]$healthFirst.loop_sequence -eq 0 -or
        [uint64]$healthSecond.loop_sequence -le
            [uint64]$healthFirst.loop_sequence -or
        [uint64]$healthSecond.connected_mask -gt 15 -or
        [uint64]$healthSecond.recovery_mask -gt 15) {
        throw "Authority process health was invalid or stopped advancing."
    }

    for ($seat = 1; $seat -lt $Players; ++$seat) {
        $peer = $peers[$seat - 1]
        # A resumed witness runs for Seconds after its first canonical frame,
        # and those witnesses are restarted sequentially after seat 0.  The
        # translated client can therefore finish before the later witnesses
        # have completed their own timed observation window.
        $peerExitTimeout = if ($allSeatCycles -ne 0) {
            ($Seconds + 30) * 1000
        } else { 15000 }
        if (-not $peer.WaitForExit($peerExitTimeout)) {
            throw "Protocol witness seat $seat did not exit."
        }
        $peer.WaitForExit()
        $peerError = [string](Get-Content $peerErrorPaths[$seat] -Raw)
        $peerLog = [string](Get-Content $peerOutputPaths[$seat] -Raw)
        $frameMatch = [regex]::Match($peerLog, "canonical_frames=([0-9]+)")
        $echoMatch = [regex]::Match($peerLog, "echoed_inputs=([0-9]+)")
        $recoveryWitness = $peerLog -match
            "RECOVERY_READY tick=[1-9][0-9]*"
        $normalWitnessFailed = $allSeatCycles -eq 0 -and (
            -not $frameMatch.Success -or -not $echoMatch.Success -or
            [uint32]$frameMatch.Groups[1].Value -lt 30 -or
            [uint32]$echoMatch.Groups[1].Value -lt 10)
        if (-not [string]::IsNullOrWhiteSpace($peerError) -or
            ($allSeatCycles -ne 0 -and -not $recoveryWitness) -or
            $normalWitnessFailed) {
            throw "Protocol witness seat $seat did not verify canonical input/state."
        }
    }

    Write-Host "Local dedicated $Players-player loss gate passed."
    Write-Host "Loss profile: $profile"
    if ($ColdJoinAfterAuthorityRestart) {
        Write-Host "Recovery: cold client bootstrapped after authority restart"
    }
    if ($InterruptInitialBootstrap) {
        Write-Host "Recovery: interrupted tick-zero bootstrap superseded by a positive checkpoint"
    }
    if ($ClientReconnectCycles -ne 0) {
        Write-Host "Recovery: $ClientReconnectCycles authenticated client reconnect cycles"
    }
    if ($allSeatCycles -ne 0) {
        Write-Host "Recovery: all $Players authenticated seats rejoined $allSeatCycles time(s)"
    }
    if ($InjectClientDesyncTick -ne 0) {
        Write-Host "Recovery: injected client divergence repaired from authority state"
    }
    Write-Host "Evidence: $evidence"
} finally {
    foreach ($process in $processes) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit(5000) | Out-Null
        }
    }
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name])
    }
}
