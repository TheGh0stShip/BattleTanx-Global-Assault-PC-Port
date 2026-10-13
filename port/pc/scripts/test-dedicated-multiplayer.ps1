[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Rom,
    [string] $BuildDirectory = "build-pc-windows",
    [ValidateRange(8, 120)]
    [int] $Seconds = 15,
    [switch] $RenderedClient
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $root $BuildDirectory
}
$romPath = (Resolve-Path $Rom).Path
$server = Join-Path $build "BattleTanxGA.exe"
$bot = Join-Path $build "btga_dedicated_client_bot.exe"
if (-not (Test-Path $server) -or -not (Test-Path $bot)) {
    throw "Build BattleTanxGA.exe and btga_dedicated_client_bot.exe first."
}

$evidence = Join-Path $build "dedicated-evidence"
New-Item $evidence -ItemType Directory -Force | Out-Null
Get-ChildItem $evidence -File -ErrorAction SilentlyContinue | Remove-Item -Force
$authorityOut = Join-Path $evidence "authority.stdout.log"
$authorityErr = Join-Path $evidence "authority.stderr.log"
$clientOut = Join-Path $evidence "client.stdout.log"
$clientErr = Join-Path $evidence "client.stderr.log"
$processes = [System.Collections.Generic.List[Diagnostics.Process]]::new()
try {
    $authority = Start-Process $server -PassThru `
        -ArgumentList @("--dedicated-server-public", $romPath) `
        -RedirectStandardOutput $authorityOut -RedirectStandardError $authorityErr
    $processes.Add($authority)
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 100
        $lobbyLine = if (Test-Path $authorityOut) {
            Get-Content $authorityOut | Where-Object { $_ -like "ONLINE_LOBBY_ID=*" } |
                Select-Object -First 1
        }
    } until ($lobbyLine -or $authority.HasExited -or [DateTime]::UtcNow -ge $deadline)
    if (-not $lobbyLine) { throw "Dedicated authority did not publish a lobby." }
    $lobby = $lobbyLine.Substring("ONLINE_LOBBY_ID=".Length)

    $clientArguments = if ($RenderedClient) {
        @("--dedicated-join", $lobby, $romPath)
    } else {
        @("--dedicated-headless", $Seconds, $lobby, $romPath)
    }
    $client = Start-Process $server -PassThru `
        -ArgumentList $clientArguments `
        -RedirectStandardOutput $clientOut -RedirectStandardError $clientErr
    $processes.Add($client)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        $clientText = ""
        if (Test-Path $clientErr) {
            $content = Get-Content $clientErr -Raw
            if ($null -ne $content) { $clientText = [string]$content }
        }
        $bound = $clientText.Contains("BTGA_CLIENT bound authoritative seat=0")
    } until ($bound -or $client.HasExited -or [DateTime]::UtcNow -ge $deadline)
    if (-not $bound) { throw "Translated client did not bind seat 0." }

    for ($seat = 1; $seat -lt 4; ++$seat) {
        $out = Join-Path $evidence "bot-$seat.stdout.log"
        $err = Join-Path $evidence "bot-$seat.stderr.log"
        $peer = Start-Process $bot -PassThru `
            -ArgumentList @("--join", $lobby, $seat, $Seconds) `
            -RedirectStandardOutput $out -RedirectStandardError $err
        $processes.Add($peer)
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            Start-Sleep -Milliseconds 100
            $peerText = ""
            if (Test-Path $out) {
                $content = Get-Content $out -Raw
                if ($null -ne $content) { $peerText = [string]$content }
            }
            $peerBound = $peerText.Contains("BOUND_SEAT=$seat")
        } until ($peerBound -or $peer.HasExited -or [DateTime]::UtcNow -ge $deadline)
        if (-not $peerBound) {
            throw "Dedicated protocol peer did not bind seat $seat."
        }
    }
    $seat = 1
    foreach ($process in $processes | Select-Object -Skip 2) {
        $process.WaitForExit(($Seconds + 15) * 1000) | Out-Null
        $process.Refresh()
        if (-not $process.HasExited) {
            throw "Dedicated protocol peer failed (PID $($process.Id))."
        }
        $peerText = [string](Get-Content (Join-Path $evidence "bot-$seat.stdout.log") -Raw)
        if (-not $peerText.Contains("seat=$seat canonical_frames=")) {
            throw "Dedicated protocol peer $seat did not complete its authority checks."
        }
        ++$seat
    }
    if ($RenderedClient) {
        $client.Refresh()
        if ($client.HasExited) {
            throw "Rendered translated client exited during the soak."
        }
    } else {
        $client.WaitForExit(($Seconds + 15) * 1000) | Out-Null
        $client.Refresh()
        if (-not $client.HasExited) {
            throw "Translated dedicated client failed."
        }
    }
    $clientLog = ""
    $content = Get-Content $clientErr -Raw
    if ($null -ne $content) { $clientLog = [string]$content }
    $hasInitialSnapshot =
        $clientLog.Contains("BTGA_CLIENT applied snapshot tick=0") -or
        $clientLog.Contains("BTGA_CLIENT confirmed snapshot tick=0")
    $completedExpectedRun = $RenderedClient -or
        $clientLog.Contains("Headless boot window elapsed")
    if (-not $hasInitialSnapshot -or -not $completedExpectedRun) {
        throw "Translated client did not apply authority state and complete its run."
    }
    if ($clientLog.Contains("BTGA_CLIENT DESYNC")) {
        throw "Translated client diverged from the dedicated authority. See $clientErr."
    }
    if ($clientLog.Contains("BTGA_CLIENT authority timeout")) {
        throw "Translated client stopped receiving canonical authority frames. See $clientErr."
    }
    $clientKind = if ($RenderedClient) { "rendered" } else { "headless" }
    Write-Host "Dedicated 4-player $clientKind test passed. Lobby: $lobby"
    Write-Host "Evidence: $evidence"
} finally {
    foreach ($process in $processes) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit(5000) | Out-Null
        }
    }
}
