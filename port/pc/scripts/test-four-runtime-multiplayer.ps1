[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Rom,
    [Parameter(Mandatory = $true)]
    [string] $NetworkingRoot,
    [string] $BuildDirectory = "build-pc-windows",
    [string] $NodeExecutable = "node",
    [ValidateRange(60, 3600)]
    [int] $Seconds = 1800,
    [ValidateRange(2, 1000)]
    [int] $DropEvery = 50,
    [ValidateRange(0, 5000)]
    [int] $OneWayDelayMilliseconds = 50,
    [switch] $SkipMatchTransition,
    [ValidateRange(1024, 65535)]
    [int] $RelayPort = 30421
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$networking = (Resolve-Path $NetworkingRoot).Path
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    (Resolve-Path $BuildDirectory).Path
} else {
    (Resolve-Path (Join-Path $root $BuildDirectory)).Path
}
$romPath = (Resolve-Path $Rom).Path
$game = Join-Path $build "BattleTanxGA.exe"
$relay = Join-Path $networking "server/relay/src/server.js"
$fixtureGenerator = Join-Path $PSScriptRoot "create-local-dedicated-fixture.js"
$node = (Get-Command $NodeExecutable -ErrorAction Stop).Source
foreach ($required in @($game, $relay, $fixtureGenerator)) {
    if (-not (Test-Path $required -PathType Leaf)) {
        throw "Required acceptance-test file is missing: $required"
    }
}

$evidenceRoot = Join-Path $build "four-runtime-evidence"
$runId = "run-{0}-{1}" -f [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssfffZ"),
    ([Guid]::NewGuid().ToString("N").Substring(0, 8))
$evidence = Join-Path $evidenceRoot $runId
New-Item $evidence -ItemType Directory -Force | Out-Null
Set-Content (Join-Path $evidenceRoot "latest.txt") $evidence -Encoding utf8

$environmentNames = @(
    "BTGA_RELAY_PUBLIC_KEY", "BTGA_RELAY_PORT", "BTGA_DEDICATED_SESSION",
    "BTGA_DEDICATED_RELAY_HOST", "BTGA_DEDICATED_RELAY_PORT",
    "BTGA_DEDICATED_TOKEN", "BTGA_DEDICATED_PLAYERS",
    "BTGA_DEDICATED_STATE_DIR", "BTGA_DATA_DIR", "BTGA_NET_TEST_FAULTS",
    "BTGA_NET_TEST_DROP_OUTGOING_EVERY", "BTGA_NET_TEST_DROP_INCOMING_EVERY",
    "BTGA_NET_TEST_DELAY_MS", "BTGA_NET_TEST_SCRIPTED_INPUT",
    "BTGA_NET_TEST_FORCE_MATCH_TRANSITION"
)
$previous = @{}
foreach ($name in $environmentNames) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name)
}
$processes = [System.Collections.Generic.List[Diagnostics.Process]]::new()
$clients = [System.Collections.Generic.List[Diagnostics.Process]]::new()

try {
    $fixtureFile = Join-Path $evidence "fixture.json"
    & $node $fixtureGenerator $fixtureFile $networking $RelayPort "127.0.0.1"
    if ($LASTEXITCODE -ne 0) { throw "Local credential generation failed." }
    $fixture = Get-Content $fixtureFile -Raw | ConvertFrom-Json
    if ($fixture.client_tokens.Count -lt 4) {
        throw "Fixture did not contain four client credentials."
    }

    $env:BTGA_RELAY_PUBLIC_KEY = [string]$fixture.relay_public_key
    $env:BTGA_RELAY_PORT = [string]$RelayPort
    $relayProcess = Start-Process $node -PassThru -WindowStyle Hidden `
        -ArgumentList @('"' + $relay + '"') `
        -RedirectStandardOutput (Join-Path $evidence "relay.stdout.log") `
        -RedirectStandardError (Join-Path $evidence "relay.stderr.log")
    $processes.Add($relayProcess)
    Start-Sleep -Milliseconds 500
    if ($relayProcess.HasExited) { throw "Local UDP relay exited during startup." }

    $env:BTGA_DEDICATED_SESSION = [string]$fixture.session
    $env:BTGA_DEDICATED_RELAY_HOST = [string]$fixture.relay_host
    $env:BTGA_DEDICATED_RELAY_PORT = [string]$fixture.relay_port
    $env:BTGA_DEDICATED_PLAYERS = "4"
    $env:BTGA_DEDICATED_STATE_DIR = Join-Path $evidence "authority-state"
    New-Item $env:BTGA_DEDICATED_STATE_DIR -ItemType Directory | Out-Null
    $env:BTGA_NET_TEST_FAULTS = "1"
    $env:BTGA_NET_TEST_DROP_OUTGOING_EVERY = [string]$DropEvery
    $env:BTGA_NET_TEST_DROP_INCOMING_EVERY = "0"
    $env:BTGA_NET_TEST_DELAY_MS = [string]$OneWayDelayMilliseconds
    $env:BTGA_NET_TEST_SCRIPTED_INPUT = "1"
    $env:BTGA_NET_TEST_FORCE_MATCH_TRANSITION = if ($SkipMatchTransition) {
        $null
    } else {
        "1"
    }

    $quotedRom = '"' + $romPath.Replace('"', '\"') + '"'
    # Each real translated client must first receive a roughly 195 KiB
    # checkpoint through the same impaired relay. Do not count that bounded
    # admission work against the requested gameplay/transition duration.
    $clientDuration = [Math]::Min(3600, $Seconds + 120)
    $env:BTGA_DEDICATED_TOKEN = [string]$fixture.authority_token
    $authority = Start-Process $game -PassThru -WindowStyle Hidden `
        -ArgumentList @("--dedicated-server", $quotedRom) `
        -RedirectStandardOutput (Join-Path $evidence "authority.stdout.log") `
        -RedirectStandardError (Join-Path $evidence "authority.stderr.log")
    $processes.Add($authority)
    Start-Sleep -Milliseconds 750

    for ($seat = 0; $seat -lt 4; ++$seat) {
        $env:BTGA_DATA_DIR = Join-Path $evidence ("client-{0}-data" -f $seat)
        New-Item $env:BTGA_DATA_DIR -ItemType Directory | Out-Null
        $env:BTGA_DEDICATED_TOKEN = [string]$fixture.client_tokens[$seat]
        $client = Start-Process $game -PassThru -WindowStyle Hidden `
            -ArgumentList @("--dedicated-headless-direct", [string]$clientDuration, $quotedRom) `
            -RedirectStandardOutput (Join-Path $evidence ("client-{0}.stdout.log" -f $seat)) `
            -RedirectStandardError (Join-Path $evidence ("client-{0}.stderr.log" -f $seat))
        # Force System.Diagnostics.Process to cache a native process handle
        # while the child is alive. Without this, Windows PowerShell can leave
        # ExitCode null after a redirected child has already terminated.
        $null = $client.Handle
        $processes.Add($client)
        $clients.Add($client)
    }

    $readyDeadline = [DateTime]::UtcNow.AddSeconds(90)
    $authorityLogPath = Join-Path $evidence "authority.stderr.log"
    do {
        Start-Sleep -Milliseconds 200
        $authorityLog = ""
        if (Test-Path $authorityLogPath) {
            $authorityLog = [string]::Join(
                [Environment]::NewLine,
                @(Get-Content $authorityLogPath -ErrorAction SilentlyContinue))
        }
        $ready = $authorityLog.Contains("BTGA_AUTHORITY all 4 seats ready")
    } until ($ready -or $authority.HasExited -or
        $clients.Where({ $_.HasExited }).Count -ne 0 -or
        [DateTime]::UtcNow -ge $readyDeadline)
    if (-not $ready) { throw "Four translated clients did not become ready." }

    foreach ($client in $clients) {
        if (-not $client.WaitForExit(($clientDuration + 120) * 1000)) {
            throw "A translated client did not complete its bounded run."
        }
        # On Windows, Process.ExitCode can remain unpopulated after the timed
        # overload when stdout/stderr are redirected. The parameterless wait
        # completes asynchronous stream teardown and makes the exit code
        # authoritative before it is inspected.
        $client.WaitForExit()
        $client.Refresh()
        $exitCode = $client.ExitCode
        if ($exitCode -ne 0) {
            throw "A translated client exited with code $exitCode."
        }
    }

    $clientTicks = @()
    $clientDigestMaps = @()
    $clientLastDesyncTicks = @()
    for ($seat = 0; $seat -lt 4; ++$seat) {
        $path = Join-Path $evidence ("client-{0}.stderr.log" -f $seat)
        $log = [string](Get-Content $path -Raw)
        if ($log -match "authority timeout|recovery quarantined") {
            throw "Translated client seat $seat timed out or was quarantined."
        }
        $desyncMatch = [regex]::Matches(
            $log, "BTGA_CLIENT DESYNC tick=([0-9]+)") | Select-Object -Last 1
        $clientLastDesyncTicks += if ($desyncMatch) {
            [uint32]$desyncMatch.Groups[1].Value
        } else {
            $null
        }
        $tickMatch = [regex]::Matches($log, "dedicated_tick=([0-9]+)") | Select-Object -Last 1
        if (-not $tickMatch) { throw "Translated client seat $seat did not report its final tick." }
        $clientTicks += [uint32]$tickMatch.Groups[1].Value
        $digestMap = @{}
        foreach ($match in [regex]::Matches(
            $log, "BTGA_CLIENT tick=([0-9]+) digest=([0-9a-f]{16})")) {
            $digestMap[[uint32]$match.Groups[1].Value] =
                $match.Groups[2].Value
        }
        if ($digestMap.Count -eq 0) {
            throw "Translated client seat $seat did not report canonical digests."
        }
        $clientDigestMaps += ,$digestMap
    }
    $authorityLog = [string](Get-Content $authorityLogPath -Raw)
    $authorityDigests = @{}
    foreach ($match in [regex]::Matches(
        $authorityLog, "BTGA_AUTHORITY tick=([0-9]+) digest=([0-9a-f]{16})")) {
        $authorityDigests[[uint32]$match.Groups[1].Value] =
            $match.Groups[2].Value
    }
    if ($authorityDigests.Count -eq 0) {
        throw "The authority did not report canonical progress."
    }
    $authorityTick = [uint32](
        $authorityDigests.Keys | Measure-Object -Maximum).Maximum
    # A required transition deliberately spends a bounded interval in the
    # retail results outer loop, where no canonical gameplay ticks exist.
    # Keep the sustained-progress gate, but allow at most twenty seconds of
    # that explicit transition instead of treating it as a simulation stall.
    $requiredProgress = [Math]::Max(300, ($Seconds * 20) -
        ($(if ($SkipMatchTransition) { 0 } else { 400 })))
    if ($authorityTick -lt $requiredProgress) {
        throw "The authority failed to sustain canonical progress after bootstrap."
    }
    $minimumClientTick = [uint32](
        ($clientTicks | Measure-Object -Minimum).Minimum)
    if ($minimumClientTick + 60 -lt $authorityTick) {
        throw "At least one translated client fell more than two seconds behind the authority."
    }
    $commonTicks = @($clientDigestMaps[0].Keys | Where-Object {
        $tick = [uint32]$_
        $authorityDigests.ContainsKey($tick) -and
        $clientDigestMaps[1].ContainsKey($tick) -and
        $clientDigestMaps[2].ContainsKey($tick) -and
        $clientDigestMaps[3].ContainsKey($tick)
    } | Sort-Object -Descending)
    if ($commonTicks.Count -eq 0) {
        throw "The authority and four clients reported no common canonical tick."
    }
    $verifiedTick = [uint32]$commonTicks[0]
    $verifiedDigest = $authorityDigests[$verifiedTick]
    foreach ($map in $clientDigestMaps) {
        if ($map[$verifiedTick] -ne $verifiedDigest) {
            throw "A translated client disagreed with the authority at tick $verifiedTick."
        }
    }
    for ($seat = 0; $seat -lt 4; ++$seat) {
        $lastDesyncTick = $clientLastDesyncTicks[$seat]
        if ($null -ne $lastDesyncTick -and $lastDesyncTick -ge $verifiedTick) {
            throw "Translated client seat $seat did not prove recovery after its last desync."
        }
    }
    if ($verifiedTick + 60 -lt $authorityTick) {
        throw "The latest verified common tick fell more than two seconds behind the authority."
    }
    if (-not $SkipMatchTransition -and
        ($authorityLog -notmatch "BTGA_AUTHORITY mode transition 3->(4|6) tick=[1-9][0-9]*" -or
         $authorityLog -notmatch "BTGA_AUTHORITY mode transition (4|6)->3 tick=[1-9][0-9]*")) {
        throw "The four-runtime run did not cross a complete match transition."
    }
    $faultText = "outgoing=$DropEvery incoming=0 delay_ms=$OneWayDelayMilliseconds"
    if (-not $authorityLog.Contains("BTGA_AUTHORITY test UDP loss $faultText")) {
        throw "Authority did not activate the required impairment profile."
    }
    $timingMatch = [regex]::Matches(
        $authorityLog, "stalls=([0-9]+) max_lag_us=([0-9]+)") |
        Select-Object -Last 1
    if (-not $timingMatch) { throw "Authority did not report timing health." }
    $result = [ordered]@{
        passed = $true
        players = 4
        seconds = $Seconds
        simulated_rtt_ms = 2 * $OneWayDelayMilliseconds
        drop_every = $DropEvery
        match_transition_required = -not $SkipMatchTransition
        client_ticks = $clientTicks
        client_last_desync_ticks = $clientLastDesyncTicks
        verified_common_tick = $verifiedTick
        verified_common_digest = $verifiedDigest
        simulation_stalls = [uint64]$timingMatch.Groups[1].Value
        maximum_tick_lag_microseconds = [uint64]$timingMatch.Groups[2].Value
        evidence = $evidence
    }
    $result | ConvertTo-Json -Depth 4 | Set-Content `
        (Join-Path $evidence "result.json") -Encoding utf8
    Write-Host "Four-runtime dedicated multiplayer gate passed."
    Write-Host ($result | ConvertTo-Json -Compress)
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
