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
    [ValidateRange(2, 1000)]
    [int] $DropOutgoingEvery = 10,
    [ValidateRange(2, 1000)]
    [int] $DropIncomingEvery = 10,
    [ValidateRange(1024, 65535)]
    [int] $RelayPort = 30421
)

$ErrorActionPreference = "Stop"
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
    "BTGA_DEDICATED_PLAYERS", "BTGA_NET_TEST_FAULTS",
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

    $env:BTGA_DEDICATED_TOKEN = [string]$fixture.client_tokens[0]
    $client = Start-Process $game -PassThru -WindowStyle Minimized `
        -ArgumentList @("--dedicated-headless-direct", [string]$Seconds, $quotedRom) `
        -RedirectStandardOutput (Join-Path $evidence "client.stdout.log") `
        -RedirectStandardError (Join-Path $evidence "client.stderr.log")
    $processes.Add($client)

    # Protocol witnesses remain reliable so the deterministic fault profile
    # targets the translated authority/client recovery path rather than token
    # admission. Each witness sends a distinct input and verifies its echo.
    Start-Sleep -Milliseconds 250
    for ($seat = 1; $seat -lt $Players; ++$seat) {
        $env:BTGA_NET_TEST_FAULTS = $null
        $env:BTGA_DEDICATED_TOKEN = [string]$fixture.client_tokens[$seat]
        $peer = Start-Process $bot -PassThru -WindowStyle Minimized `
            -ArgumentList @([string]$seat, [string]$Seconds) `
            -RedirectStandardOutput (Join-Path $evidence "bot-$seat.stdout.log") `
            -RedirectStandardError (Join-Path $evidence "bot-$seat.stderr.log")
        $processes.Add($peer)
        $peers.Add($peer)
        $env:BTGA_NET_TEST_FAULTS = "1"
    }

    if (-not $client.WaitForExit(($Seconds + 30) * 1000)) {
        throw "Translated client did not complete its timed run."
    }
    $client.WaitForExit()
    $clientLog = [string](Get-Content (Join-Path $evidence "client.stderr.log") -Raw)
    if ($clientLog -notmatch "BTGA_CLIENT (applied|confirmed) snapshot tick=0" -or
        -not $clientLog.Contains("Headless boot window elapsed")) {
        throw "Translated client did not install the bootstrap and complete its run."
    }
    if ($clientLog.Contains("BTGA_CLIENT DESYNC") -or
        $clientLog.Contains("BTGA_CLIENT authority timeout")) {
        throw "Translated client diverged or lost the authority."
    }
    $profile = "outgoing=$DropOutgoingEvery incoming=$DropIncomingEvery"
    if (-not $clientLog.Contains("BTGA_CLIENT test UDP loss $profile")) {
        throw "Translated client did not activate the requested loss profile."
    }

    $authorityLog = [string](Get-Content `
        (Join-Path $evidence "authority.stderr.log") -Raw)
    if (-not $authorityLog.Contains("BTGA_AUTHORITY all $Players seats ready") -or
        -not $authorityLog.Contains("BTGA_AUTHORITY test UDP loss $profile")) {
        throw "Authority did not reach the requested lossy $Players-player session."
    }

    for ($seat = 1; $seat -lt $Players; ++$seat) {
        $peer = $peers[$seat - 1]
        if (-not $peer.WaitForExit(15000)) {
            throw "Protocol witness seat $seat did not exit."
        }
        $peer.WaitForExit()
        $peerError = [string](Get-Content `
            (Join-Path $evidence "bot-$seat.stderr.log") -Raw)
        $peerLog = [string](Get-Content `
            (Join-Path $evidence "bot-$seat.stdout.log") -Raw)
        if (-not [string]::IsNullOrWhiteSpace($peerError) -or
            $peerLog -notmatch "canonical_frames=[3-9][0-9]" -or
            $peerLog -notmatch "echoed_inputs=[1-9][0-9]") {
            throw "Protocol witness seat $seat did not verify canonical input/state."
        }
    }

    Write-Host "Local dedicated $Players-player loss gate passed."
    Write-Host "Loss profile: $profile"
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
