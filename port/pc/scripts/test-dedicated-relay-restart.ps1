[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Rom,
    [Parameter(Mandatory = $true)]
    [string] $NetworkingRoot,
    [string] $NodeExecutable = "node",
    [string] $BuildDirectory = "build-pc-windows",
    [ValidateRange(120, 300)]
    [int] $Seconds = 150,
    [ValidateRange(1024, 65534)]
    [int] $MasterPort = 30419,
    [ValidateRange(1024, 65535)]
    [int] $RelayPort = 30420
)

$ErrorActionPreference = "Stop"
if ($MasterPort -eq $RelayPort) { throw "MasterPort and RelayPort must differ." }
$root = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$networking = (Resolve-Path $NetworkingRoot).Path
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    (Resolve-Path $BuildDirectory).Path
} else {
    (Resolve-Path (Join-Path $root $BuildDirectory)).Path
}
$romPath = (Resolve-Path $Rom).Path
$game = Join-Path $build "BattleTanxGA.exe"
$masterScript = Join-Path $networking "server/master/src/local_server.js"
$relayScript = Join-Path $networking "server/relay/src/server.js"
$node = (Get-Command $NodeExecutable -ErrorAction Stop).Source
foreach ($required in @($game, $masterScript, $relayScript)) {
    if (-not (Test-Path $required -PathType Leaf)) {
        throw "Required relay-restart fixture is missing: $required"
    }
}

$evidenceRoot = Join-Path $build "relay-restart-evidence"
New-Item $evidenceRoot -ItemType Directory -Force | Out-Null
$evidence = Join-Path $evidenceRoot ("run-{0}-{1}" -f `
    [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssfffZ"), $PID)
New-Item $evidence -ItemType Directory | Out-Null
$readyFile = Join-Path $evidence "master-ready.json"
$stateDirectory = Join-Path $evidence "authority-state"
New-Item $stateDirectory -ItemType Directory | Out-Null

$environmentNames = @(
    "BTGA_LOCAL_MASTER_HOST", "BTGA_LOCAL_MASTER_PORT",
    "BTGA_LOCAL_MASTER_READY_FILE", "BTGA_MASTER_URL",
    "BTGA_RELAY_HOST", "BTGA_RELAY_PORT", "BTGA_RELAY_PUBLIC_KEY",
    "BTGA_DEDICATED_PLAYERS", "BTGA_DEDICATED_STATE_DIR",
    "BTGA_DEDICATED_RESUME_FILE", "BTGA_DATA_DIR"
)
$previous = @{}
foreach ($name in $environmentNames) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name)
}
$owned = [System.Collections.Generic.List[Diagnostics.Process]]::new()

function Start-HiddenProcess(
    [string] $File, [string[]] $Arguments,
    [string] $Stdout, [string] $Stderr) {
    $process = Start-Process $File -PassThru -WindowStyle Minimized `
        -ArgumentList $Arguments -RedirectStandardOutput $Stdout `
        -RedirectStandardError $Stderr
    $owned.Add($process)
    return $process
}

function Stop-OwnedProcess([Diagnostics.Process] $Process) {
    if ($Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force
        $Process.WaitForExit(5000) | Out-Null
    }
}

function Wait-Log(
    [string] $Path, [string] $Pattern, [int] $TimeoutSeconds,
    [Diagnostics.Process[]] $Processes) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 100
        $text = if (Test-Path $Path -PathType Leaf) {
            [string](Get-Content $Path -Raw)
        } else { "" }
        if ($text -match $Pattern) { return $text }
        foreach ($process in $Processes) {
            if ($process.HasExited) {
                throw "Process $($process.Id) exited while waiting for '$Pattern'."
            }
        }
    } until ([DateTime]::UtcNow -ge $deadline)
    throw "Timed out waiting for '$Pattern' in $Path."
}

try {
    $env:BTGA_LOCAL_MASTER_HOST = "127.0.0.1"
    $env:BTGA_LOCAL_MASTER_PORT = [string]$MasterPort
    $env:BTGA_LOCAL_MASTER_READY_FILE = $readyFile
    $env:BTGA_RELAY_HOST = "127.0.0.1"
    $env:BTGA_RELAY_PORT = [string]$RelayPort
    $master = Start-HiddenProcess $node @('"' + $masterScript + '"') `
        (Join-Path $evidence "master.stdout.log") `
        (Join-Path $evidence "master.stderr.log")
    $readyDeadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path $readyFile -PathType Leaf) -and
           [DateTime]::UtcNow -lt $readyDeadline -and -not $master.HasExited) {
        Start-Sleep -Milliseconds 50
    }
    if (-not (Test-Path $readyFile -PathType Leaf)) {
        throw "Local master did not publish readiness."
    }
    $ready = Get-Content $readyFile -Raw | ConvertFrom-Json
    $env:BTGA_MASTER_URL = [string]$ready.url
    $env:BTGA_RELAY_PUBLIC_KEY = [string]$ready.relay_public_key

    $relayLog = Join-Path $evidence "relay-1.stdout.log"
    $relay = Start-HiddenProcess $node @('"' + $relayScript + '"') $relayLog `
        (Join-Path $evidence "relay-1.stderr.log")
    Wait-Log $relayLog "BTGA UDP relay listening" 10 @($master, $relay) | Out-Null

    $env:BTGA_DEDICATED_PLAYERS = "4"
    $env:BTGA_DEDICATED_STATE_DIR = $stateDirectory
    $env:BTGA_DATA_DIR = Join-Path $evidence "authority-data"
    $authorityOut = Join-Path $evidence "authority.stdout.log"
    $authorityErr = Join-Path $evidence "authority.stderr.log"
    $authority = Start-HiddenProcess $game @(
        "--dedicated-server-public", '"' + $romPath + '"') `
        $authorityOut $authorityErr
    $authorityText = Wait-Log $authorityOut "ONLINE_LOBBY_ID=" 20 @($master, $relay, $authority)
    $lobbyMatch = [regex]::Match($authorityText, "ONLINE_LOBBY_ID=([A-Za-z0-9_-]+)")
    if (-not $lobbyMatch.Success) { throw "Authority did not expose its lobby id." }
    $lobby = $lobbyMatch.Groups[1].Value

    $clients = @()
    $clientErrors = @()
    for ($seat = 0; $seat -lt 4; ++$seat) {
        $clientDirectory = Join-Path $evidence "client-$seat"
        New-Item $clientDirectory -ItemType Directory | Out-Null
        $env:BTGA_DATA_DIR = $clientDirectory
        $env:BTGA_DEDICATED_RESUME_FILE = Join-Path $clientDirectory "resume.token"
        $errorPath = Join-Path $evidence "client-$seat.stderr.log"
        $clientErrors += $errorPath
        $client = Start-HiddenProcess $game @(
            "--dedicated-headless", [string]$Seconds, $lobby,
            '"' + $romPath + '"') `
            (Join-Path $evidence "client-$seat.stdout.log") $errorPath
        $clients += $client
    }
    $env:BTGA_DEDICATED_RESUME_FILE = $null
    $env:BTGA_DATA_DIR = $null

    Wait-Log $authorityErr "BTGA_AUTHORITY all 4 seats ready" 60 `
        (@($master, $relay, $authority) + $clients) | Out-Null
    $beforeText = Wait-Log $authorityErr "BTGA_AUTHORITY tick=[1-9][0-9][0-9]" 60 `
        (@($master, $relay, $authority) + $clients)
    $beforeTicks = [regex]::Matches($beforeText, "BTGA_AUTHORITY tick=([0-9]+)")
    $beforeTick = [uint32]$beforeTicks[$beforeTicks.Count - 1].Groups[1].Value
    $authorityBindingsBefore = [regex]::Matches(
        $beforeText, "BTGA_AUTHORITY relay binding restored").Count
    $clientReplayCountsBefore = @()
    for ($seat = 0; $seat -lt 4; ++$seat) {
        $initialClientText = [string](Get-Content $clientErrors[$seat] -Raw)
        $clientReplayCountsBefore += [regex]::Matches(
            $initialClientText, "BTGA_CLIENT journal replay complete").Count
    }

    # Do not restart the data plane while the four seats are merely
    # provisional. Waiting for the authority's seat-mask heartbeat proves the
    # control plane has confirmed every live player and makes this exercise the
    # same lease path used by the public service.
    $seatDeadline = [DateTime]::UtcNow.AddSeconds(25)
    $publicPlayers = 0
    do {
        Start-Sleep -Milliseconds 250
        try {
            $status = Invoke-RestMethod -Method Get -Uri `
                ([string]$ready.url + "/status") -TimeoutSec 2
            $publicPlayers = [int]$status.players
        } catch {
            $publicPlayers = 0
        }
    } until ($publicPlayers -eq 4 -or $master.HasExited -or
        $authority.HasExited -or [DateTime]::UtcNow -ge $seatDeadline)
    if ($publicPlayers -ne 4) {
        throw "Authority did not confirm all four seat leases before relay restart."
    }

    Stop-OwnedProcess $relay
    Start-Sleep -Seconds 3
    $relayLog2 = Join-Path $evidence "relay-2.stdout.log"
    $relay = Start-HiddenProcess $node @('"' + $relayScript + '"') $relayLog2 `
        (Join-Path $evidence "relay-2.stderr.log")
    Wait-Log $relayLog2 "BTGA UDP relay listening" 10 @($master, $relay, $authority) | Out-Null

    $authorityRecoveryDeadline = [DateTime]::UtcNow.AddSeconds(35)
    do {
        Start-Sleep -Milliseconds 100
        $authorityRecoveryText = [string](Get-Content $authorityErr -Raw)
        $authorityBindingsAfter = [regex]::Matches(
            $authorityRecoveryText,
            "BTGA_AUTHORITY relay binding restored").Count
    } until ($authorityBindingsAfter -gt $authorityBindingsBefore -or
        $master.HasExited -or $relay.HasExited -or $authority.HasExited -or
        [DateTime]::UtcNow -ge $authorityRecoveryDeadline)
    if ($authorityBindingsAfter -le $authorityBindingsBefore) {
        throw "Authority did not establish a new binding after relay restart."
    }
    for ($seat = 0; $seat -lt 4; ++$seat) {
        $clientText = Wait-Log $clientErrors[$seat] `
            "BTGA_CLIENT reclaiming relay seat after authority timeout" 40 `
            @($master, $relay, $authority, $clients[$seat])
        if ($clientText -notmatch "BTGA_CLIENT relay binding restored") {
            $clientText = Wait-Log $clientErrors[$seat] `
                "BTGA_CLIENT relay binding restored" 20 `
                @($master, $relay, $authority, $clients[$seat])
        }
        $replayDeadline = [DateTime]::UtcNow.AddSeconds(45)
        do {
            Start-Sleep -Milliseconds 100
            $clientText = [string](Get-Content $clientErrors[$seat] -Raw)
            $replaysAfter = [regex]::Matches(
                $clientText, "BTGA_CLIENT journal replay complete").Count
        } until ($replaysAfter -gt $clientReplayCountsBefore[$seat] -or
            $clients[$seat].HasExited -or $authority.HasExited -or
            [DateTime]::UtcNow -ge $replayDeadline)
        if ($replaysAfter -le $clientReplayCountsBefore[$seat]) {
            throw "Client seat $seat rebound but did not replay to canonical head."
        }
    }

    $advanceDeadline = [DateTime]::UtcNow.AddSeconds(45)
    $afterTick = $beforeTick
    do {
        Start-Sleep -Milliseconds 250
        $authorityText = [string](Get-Content $authorityErr -Raw)
        $ticks = [regex]::Matches($authorityText, "BTGA_AUTHORITY tick=([0-9]+)")
        if ($ticks.Count -ne 0) {
            $afterTick = [uint32]$ticks[$ticks.Count - 1].Groups[1].Value
        }
    } until ($afterTick -gt $beforeTick -or $authority.HasExited -or
        [DateTime]::UtcNow -ge $advanceDeadline)
    if ($afterTick -le $beforeTick) {
        throw "Authority did not advance after relay recovery."
    }
    foreach ($path in $clientErrors) {
        $text = [string](Get-Content $path -Raw)
        if ($text.Contains("recovery quarantined") -or $text.Contains("DESYNC")) {
            throw "A client diverged or quarantined during relay recovery: $path"
        }
    }
    [ordered]@{
        schema = 1
        lobby = $lobby
        seats = 4
        before_tick = $beforeTick
        after_tick = $afterTick
        relay_restarted = $true
        clients_rebound = 4
        passed = $true
    } | ConvertTo-Json | Set-Content `
        (Join-Path $evidence "result.json") -Encoding UTF8
    Write-Host "Four-seat dedicated relay restart gate passed."
    Write-Host "Authority advanced from tick $beforeTick to $afterTick."
    Write-Host "Evidence: $evidence"
} finally {
    foreach ($process in $owned) { Stop-OwnedProcess $process }
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name])
    }
}
