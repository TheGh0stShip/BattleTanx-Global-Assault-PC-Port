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
    [switch] $ColdJoinAfterAuthorityRestart,
    [ValidateRange(0, 3)]
    [int] $ClientReconnectCycles = 0,
    [switch] $ReconnectEverySeat,
    [ValidateRange(0, 3)]
    [int] $AllSeatReconnectCycles = 0,
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
if (($ColdJoinAfterAuthorityRestart -and $ClientReconnectCycles -ne 0) -or
    ($ColdJoinAfterAuthorityRestart -and $allSeatCycles -ne 0) -or
    ($ClientReconnectCycles -ne 0 -and $allSeatCycles -ne 0)) {
    throw "Authority restart, translated-client reconnect, and all-seat reconnect are separate gates."
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
    "BTGA_DEDICATED_PLAYERS", "BTGA_DEDICATED_STATE_DIR",
    "BTGA_NET_TEST_FAULTS",
    "BTGA_NET_TEST_DROP_OUTGOING_EVERY", "BTGA_NET_TEST_DROP_INCOMING_EVERY"
)
$previous = @{}
foreach ($name in $environmentNames) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name)
}
$processes = [System.Collections.Generic.List[Diagnostics.Process]]::new()
$peers = [System.Collections.Generic.List[Diagnostics.Process]]::new()
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
    if ($ColdJoinAfterAuthorityRestart) {
        $stateDirectory = Join-Path $evidence "authority-state"
        New-Item $stateDirectory -ItemType Directory | Out-Null
        $env:BTGA_DEDICATED_STATE_DIR = $stateDirectory
    } else {
        $env:BTGA_DEDICATED_STATE_DIR = $null
    }
    $env:BTGA_NET_TEST_FAULTS = "1"
    $env:BTGA_NET_TEST_DROP_OUTGOING_EVERY = [string]$DropOutgoingEvery
    $env:BTGA_NET_TEST_DROP_INCOMING_EVERY = [string]$DropIncomingEvery

    $env:BTGA_DEDICATED_TOKEN = [string]$fixture.authority_token
    $authority = Start-Process $game -PassThru -WindowStyle Minimized `
        -ArgumentList @("--dedicated-server", $quotedRom) `
        -RedirectStandardOutput (Join-Path $evidence "authority.stdout.log") `
        -RedirectStandardError (Join-Path $evidence "authority.stderr.log")
    $processes.Add($authority)
    Start-Sleep -Milliseconds 750

    $translatedSeconds = if ($allSeatCycles -ne 0) {
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
    $peerSeconds = if ($ColdJoinAfterAuthorityRestart) {
        # Cover the initial run, forced restart and replacement-client run,
        # then finish soon enough for the gate's bounded validation wait.
        $Seconds + 10
    } elseif ($ClientReconnectCycles -ne 0) {
        $Seconds + 15 + 5 * $ClientReconnectCycles
    } elseif ($allSeatCycles -ne 0) {
        $Seconds + 15 + 5 * $Players * $allSeatCycles
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
            -ArgumentList @("--dedicated-headless-direct", [string]$Seconds, $quotedRom) `
            -RedirectStandardOutput (Join-Path $evidence "client-restart.stdout.log") `
            -RedirectStandardError $clientErrorPath
        $processes.Add($client)
    } elseif ($ClientReconnectCycles -ne 0) {
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
        for ($cycle = 1; $cycle -le $allSeatCycles; ++$cycle) {
            if (-not $client.HasExited) {
                Stop-Process -Id $client.Id -Force
                $client.WaitForExit(5000) | Out-Null
            }
            $clientErrorPath = Join-Path $evidence `
                ("client-reconnect-cycle-{0}-seat-0.stderr.log" -f $cycle)
            $clientErrorPaths.Add($clientErrorPath)
            $env:BTGA_DEDICATED_TOKEN =
                [string]$fixture.client_reconnect_tokens[0][$cycle - 1]
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
            $expectedBarrier = (($cycle - 1) * $Players) + 1
            $barrierDeadline = [DateTime]::UtcNow.AddSeconds(60)
            do {
                Start-Sleep -Milliseconds 100
                $liveAuthorityLog =
                    [string](Get-Content $authorityErrorPaths[0] -Raw)
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
                $expectedBarrier = (($cycle - 1) * $Players) + $seat + 1
                $barrierDeadline = [DateTime]::UtcNow.AddSeconds(60)
                do {
                    Start-Sleep -Milliseconds 100
                    $liveAuthorityLog =
                        [string](Get-Content $authorityErrorPaths[0] -Raw)
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
    $requiredSnapshot = if ($ColdJoinAfterAuthorityRestart -or
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
    if ($ClientReconnectCycles -ne 0 -or $allSeatCycles -ne 0) {
        $recoveryCount = [regex]::Matches(
            $authorityLog, "BTGA_AUTHORITY recovery complete head=[1-9][0-9]*").Count
        $expectedRecoveries = if ($allSeatCycles -ne 0) {
            $Players * $allSeatCycles
        } else {
            $ClientReconnectCycles
        }
        if ($recoveryCount -lt $expectedRecoveries) {
            throw "Authority did not complete every seat-recovery barrier."
        }
    }
    foreach ($path in $clientErrorPaths) {
        $log = [string](Get-Content $path -Raw)
        if ($log.Contains("BTGA_CLIENT DESYNC") -or
            $log.Contains("BTGA_CLIENT authority timeout")) {
            throw "Translated client diverged or lost the authority."
        }
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
    if ($ClientReconnectCycles -ne 0) {
        Write-Host "Recovery: $ClientReconnectCycles authenticated client reconnect cycles"
    }
    if ($allSeatCycles -ne 0) {
        Write-Host "Recovery: all $Players authenticated seats rejoined $allSeatCycles time(s)"
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
