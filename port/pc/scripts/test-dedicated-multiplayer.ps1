[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Rom,
    [string] $BuildDirectory = "build-pc-windows",
    [ValidateRange(8, 120)]
    [int] $Seconds = 15,
    [switch] $RenderedClient,
    [switch] $ReconnectClient,
    [ValidateRange(2, 30)]
    [int] $ReconnectAfterSeconds = 4,
    [ValidateRange(1, 3)]
    [int] $ReconnectCycles = 1,
    [ValidateRange(32, 1000000)]
    [int] $JournalMemoryFrames = 4096,
    [switch] $RequireDiskReplay
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
$resumeCredential = Join-Path $evidence "client-resume.credential"
$previousResumeFile = $env:BTGA_DEDICATED_RESUME_FILE
$previousJournalMemoryFrames = $env:BTGA_DEDICATED_JOURNAL_MEMORY_FRAMES
$env:BTGA_DEDICATED_RESUME_FILE = $resumeCredential
$env:BTGA_DEDICATED_JOURNAL_MEMORY_FRAMES = [string]$JournalMemoryFrames
$processes = [System.Collections.Generic.List[Diagnostics.Process]]::new()
$peers = [System.Collections.Generic.List[Diagnostics.Process]]::new()
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
    if (-not (Test-Path $resumeCredential) -or
        (Get-Item $resumeCredential).Length -eq 0) {
        throw "Translated client did not persist its rotating resume credential."
    }

    $peerSeconds = $Seconds
    if ($ReconnectClient) {
        $peerSeconds += $ReconnectCycles * ($ReconnectAfterSeconds + 20)
    }
    for ($seat = 1; $seat -lt 4; ++$seat) {
        $out = Join-Path $evidence "bot-$seat.stdout.log"
        $err = Join-Path $evidence "bot-$seat.stderr.log"
        $peer = Start-Process $bot -PassThru `
            -ArgumentList @("--join", $lobby, $seat, $peerSeconds) `
            -RedirectStandardOutput $out -RedirectStandardError $err
        $processes.Add($peer)
        $peers.Add($peer)
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
    if ($ReconnectClient) {
        if ($RenderedClient) {
            throw "-ReconnectClient currently requires the headless translated client."
        }
        for ($cycle = 1; $cycle -le $ReconnectCycles; ++$cycle) {
            $initialCredential = [string](Get-Content $resumeCredential -Raw)
            Start-Sleep -Seconds $ReconnectAfterSeconds
            if (-not $client.HasExited) {
                Stop-Process -Id $client.Id -Force
                $client.WaitForExit(5000) | Out-Null
            }
            $clientOut = Join-Path $evidence "client-resumed-$cycle.stdout.log"
            $clientErr = Join-Path $evidence "client-resumed-$cycle.stderr.log"
            $client = Start-Process $server -PassThru `
                -ArgumentList $clientArguments `
                -RedirectStandardOutput $clientOut -RedirectStandardError $clientErr
            $processes.Add($client)
            $deadline = [DateTime]::UtcNow.AddSeconds(20)
            do {
                Start-Sleep -Milliseconds 100
                $clientText = ""
                if (Test-Path $clientErr) {
                    $content = Get-Content $clientErr -Raw
                    if ($null -ne $content) { $clientText = [string]$content }
                }
                $bound = $clientText.Contains("BTGA_CLIENT bound authoritative seat=0")
                $replayed = $clientText.Contains("BTGA_CLIENT journal replay active")
                $reconverged = [regex]::IsMatch(
                    $clientText,
                    "BTGA_CLIENT confirmed snapshot tick=[1-9][0-9]*")
            } until (($bound -and $replayed -and $reconverged) -or
                $client.HasExited -or [DateTime]::UtcNow -ge $deadline)
            if (-not $bound) {
                throw "Reconnect cycle $cycle did not reclaim seat 0."
            }
            $rotatedCredential = [string](Get-Content $resumeCredential -Raw)
            if ($rotatedCredential -eq $initialCredential) {
                throw "Resume credential did not rotate in reconnect cycle $cycle."
            }
            if (-not $replayed -or -not $reconverged) {
                throw "Reconnect cycle $cycle did not replay and reconverge. See $clientErr."
            }
        }
    }
    $seat = 1
    foreach ($process in $peers) {
        $process.WaitForExit(($peerSeconds + 15) * 1000) | Out-Null
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
    if ($ReconnectClient -and
        -not $clientLog.Contains("BTGA_CLIENT journal replay active")) {
        throw "Resumed client did not receive canonical journal replay. See $clientErr."
    }
    if ($ReconnectClient -and
        -not [regex]::IsMatch($clientLog, "BTGA_CLIENT confirmed snapshot tick=[1-9][0-9]*")) {
        throw "Resumed client did not catch up and reconverge with the authority. See $clientErr."
    }
    if ($RequireDiskReplay) {
        $authorityLog = [string](Get-Content $authorityErr -Raw)
        if (-not $authorityLog.Contains("BTGA_AUTHORITY disk journal replay")) {
            throw "Authority did not replay canonical history from disk. See $authorityErr."
        }
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
    $env:BTGA_DEDICATED_RESUME_FILE = $previousResumeFile
    $env:BTGA_DEDICATED_JOURNAL_MEMORY_FRAMES = $previousJournalMemoryFrames
}
