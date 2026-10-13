[CmdletBinding()]
param(
    [string] $BuildDirectory = "build-pc-windows",
    [ValidateRange(1, 100)]
    [int] $RelayRuns = 1,
    [string] $Rom,
    [string] $ControllerPak,
    [ValidateRange(1, 300)]
    [int] $BootSeconds = 60,
    [ValidateRange(1, 100)]
    [int] $GameRuns = 5,
    [switch] $KillClientAfterEpoch,
    [ValidateRange(0, 100)]
    [int] $PeerLossRun = 0,
    [ValidateRange(5, 120)]
    [int] $ProgressTimeoutSeconds = 30,
    [switch] $SkipBuild,
    [switch] $ReplayEquivalenceProbe,
    [switch] $ShowWindows
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$buildScript = Join-Path $root "port\pc\build-windows.ps1"

# First build and run the deterministic library-level tests. When -Rom is
# supplied, the second half also starts two hidden, bounded native runtimes;
# no visible window or human input is required.
if (-not $SkipBuild) {
    & $buildScript -BuildDirectory $BuildDirectory
    if ($LASTEXITCODE -ne 0) { throw "Native headless tests failed." }
}

$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $root $BuildDirectory
}
if (-not $SkipBuild) {
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if (-not $cmake) { $cmake = Get-Command cmake -ErrorAction Stop }
    & $cmake.Source --build $build --target btga_pc_native_online_e2e --parallel
    if ($LASTEXITCODE -ne 0) { throw "Native online lifecycle test build failed." }
}

$e2e = Join-Path $build "btga_pc_native_online_e2e.exe"
for ($run = 1; $run -le $RelayRuns; ++$run) {
    Write-Host "Public relay lifecycle $run/$RelayRuns"
    & $e2e
    if ($LASTEXITCODE -ne 0) { throw "Public relay lifecycle failed on run $run." }
}

if ($Rom) {
    if ($PeerLossRun -gt $GameRuns) {
        throw "-PeerLossRun cannot be greater than -GameRuns."
    }
    $resolvedRom = (Resolve-Path $Rom).Path
    $resolvedPak = if ($ControllerPak) { (Resolve-Path $ControllerPak).Path } else { $null }
    $runtime = Join-Path $build "BattleTanxGA.exe"
    $evidenceRoot = Join-Path $build "headless-online-evidence"
    Remove-Item $evidenceRoot -Recurse -Force -ErrorAction SilentlyContinue
    $results = [System.Collections.Generic.List[object]]::new()
    for ($gameRun = 1; $gameRun -le $GameRuns; ++$gameRun) {
    $evidence = Join-Path $evidenceRoot ("run-{0:D2}" -f $gameRun)
    $hostData = Join-Path $evidence "host-data"
    $clientData = Join-Path $evidence "client-data"
    New-Item $hostData, $clientData -ItemType Directory -Force | Out-Null
    if ($resolvedPak) {
        $pakName = "battletanx-ga.n64.us.1.0_pak1.mpk"
        $hostSaves = Join-Path $hostData "saves"
        $clientSaves = Join-Path $clientData "saves"
        New-Item $hostSaves, $clientSaves -ItemType Directory -Force | Out-Null
        Copy-Item $resolvedPak (Join-Path $hostSaves $pakName)
        Copy-Item $resolvedPak (Join-Path $clientSaves $pakName)
    }
    $hostOut = Join-Path $evidence "host.stdout.log"
    $hostErr = Join-Path $evidence "host.stderr.log"
    $clientOut = Join-Path $evidence "client.stdout.log"
    $clientErr = Join-Path $evidence "client.stderr.log"
    $hostProcess = $null
    $clientProcess = $null
    $clientKilled = $false
    $injectPeerLoss = $KillClientAfterEpoch -or ($PeerLossRun -eq $gameRun)
    $lastProgressAt = [DateTime]::UtcNow
    $lastProgressSignature = ""

    try {
        if ($ReplayEquivalenceProbe) { $env:BTGA_REPLAY_EQUIVALENCE_PROBE = "1" }
        if ($ShowWindows) { $env:BTGA_HEADLESS_VISIBLE = "1" }
        $env:BTGA_DATA_DIR = $hostData
        $env:BTGA_ONLINE_LOCAL_VIEW = "host"
        $hostProcess = Start-Process -FilePath $runtime -PassThru `
            -ArgumentList @("--online-headless-host", $BootSeconds, $resolvedRom) `
            -RedirectStandardOutput $hostOut -RedirectStandardError $hostErr
        $lobbyFile = Join-Path $hostData "online-lobby.txt"
        $deadline = [DateTime]::UtcNow.AddSeconds(45)
        while (-not (Test-Path $lobbyFile) -and -not $hostProcess.HasExited -and
               [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 100
            $hostProcess.Refresh()
        }
        if (-not (Test-Path $lobbyFile)) {
            throw "Headless host did not publish a lobby ID."
        }
        $lobby = (Get-Content $lobbyFile -Raw).Trim()
        if (-not $lobby) { throw "Headless host published an empty lobby ID." }

        $env:BTGA_DATA_DIR = $clientData
        $env:BTGA_ONLINE_LOCAL_VIEW = "client"
        $clientProcess = Start-Process -FilePath $runtime -PassThru `
            -ArgumentList @("--online-headless-join", $BootSeconds, $lobby, $resolvedRom) `
            -RedirectStandardOutput $clientOut -RedirectStandardError $clientErr
        $deadline = [DateTime]::UtcNow.AddSeconds($BootSeconds + 60)
        while ((!$hostProcess.HasExited -or !$clientProcess.HasExited) -and
               [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 100
            $hostProcess.Refresh()
            $clientProcess.Refresh()
            $hostSyncCandidate = Join-Path $hostData "btga-net-sync.log"
            $clientSyncCandidate = Join-Path $clientData "btga-net-sync.log"
            $progressSignature = @($hostSyncCandidate, $clientSyncCandidate) | ForEach-Object {
                if (Test-Path $_) {
                    $item = Get-Item $_
                    "$($item.Length):$($item.LastWriteTimeUtc.Ticks)"
                } else { "missing" }
            }
            $progressSignature = $progressSignature -join "|"
            if ($progressSignature -ne $lastProgressSignature) {
                $lastProgressSignature = $progressSignature
                $lastProgressAt = [DateTime]::UtcNow
            }
            if ($injectPeerLoss -and !$clientKilled -and
                !$clientProcess.HasExited) {
                if ((Test-Path $hostSyncCandidate) -and
                    (Test-Path $clientSyncCandidate) -and
                    (Get-Content $hostSyncCandidate -Raw).Contains("GAMEPLAY_EPOCH_SYNC") -and
                    (Get-Content $clientSyncCandidate -Raw).Contains("GAMEPLAY_EPOCH_SYNC")) {
                    Stop-Process -Id $clientProcess.Id -Force
                    $clientKilled = $true
                    $clientProcess.Refresh()
                }
            }
            $peerLossRecovered = $false
            if ($injectPeerLoss -and $clientKilled -and (Test-Path $hostSyncCandidate)) {
                $peerLossRecovered =
                    (Get-Content $hostSyncCandidate -Raw) -match
                        "FRAME_BARRIER_TIMEOUT|FRAME_INPUT_TIMEOUT"
            }
            if (-not $peerLossRecovered -and
                [DateTime]::UtcNow -gt $lastProgressAt.AddSeconds($ProgressTimeoutSeconds)) {
                throw "Headless pair made no synchronization-log progress for $ProgressTimeoutSeconds seconds."
            }
        }
        if (!$hostProcess.HasExited -or !$clientProcess.HasExited) {
            throw "Headless game pair exceeded its bounded timeout."
        }
        $hostProcess.WaitForExit()
        $clientProcess.WaitForExit()
        $hostProcess.Refresh()
        $clientProcess.Refresh()
        if (($null -ne $hostProcess.ExitCode -and $hostProcess.ExitCode -ne 0) -or
            (!$injectPeerLoss -and
             ($null -ne $clientProcess.ExitCode -and $clientProcess.ExitCode -ne 0))) {
            throw "Headless runtime exited unsuccessfully (host=$($hostProcess.ExitCode), client=$($clientProcess.ExitCode))."
        }
        $hostLog = (Get-Content $hostErr -Raw)
        $clientLog = (Get-Content $clientErr -Raw)
        if (!$hostLog.Contains("Headless boot window elapsed") -or
            (!$injectPeerLoss -and
             !$clientLog.Contains("Headless boot window elapsed"))) {
            throw "Headless game pair did not reach the bounded synchronized boot window."
        }
        if ($hostLog.Contains("Headless lobby cleanup failed")) {
            throw "Headless host could not remove its temporary lobby."
        }
        if ($ReplayEquivalenceProbe) {
            foreach ($namedLog in @(@("host", $hostLog), @("client", $clientLog))) {
                if ($namedLog[1].Contains("BTGA_REPLAY_PROBE FAIL")) {
                    throw "Headless $($namedLog[0]) failed deterministic tick replay."
                }
                $passes = [regex]::Matches($namedLog[1], "BTGA_REPLAY_PROBE PASS")
                if ($passes.Count -ne 1) {
                    throw "Headless $($namedLog[0]) reported $($passes.Count) replay passes; expected exactly one."
                }
            }
        }
        $hostSyncPath = Join-Path $hostData "btga-net-sync.log"
        $clientSyncPath = Join-Path $clientData "btga-net-sync.log"
        $hostSync = if (Test-Path $hostSyncPath) { Get-Content $hostSyncPath -Raw } else { "" }
        $clientSync = if (Test-Path $clientSyncPath) { Get-Content $clientSyncPath -Raw } else { "" }
        if ($injectPeerLoss) {
            if (!$clientKilled) {
                throw "Peer-loss injection never reached a synchronized gameplay epoch."
            }
            if ($hostSync -notmatch "FRAME_BARRIER_TIMEOUT|FRAME_INPUT_TIMEOUT") {
                throw "Host did not leave its network wait after the client was terminated."
            }
            $results.Add([pscustomobject]@{
                run = $gameRun
                scenario = "peer-loss"
                passed = $true
                client_terminated = $clientKilled
                evidence = $evidence
            })
            Write-Host "Headless peer-loss recovery $gameRun/$GameRuns passed; evidence: $evidence"
            continue
        }
        if (!$hostSync.Contains("GAMEPLAY_EPOCH_SYNC") -or
            !$clientSync.Contains("GAMEPLAY_EPOCH_SYNC")) {
            throw "Scripted pair booted but did not reach synchronized gameplay."
        }
        foreach ($namedLog in @(@("host", $hostSync), @("client", $clientSync))) {
            if ($namedLog[1] -match "TIMEOUT") {
                throw "Headless $($namedLog[0]) reported a synchronization timeout."
            }
        }
        if ($hostSync.Contains("DIGEST_MISMATCH") -or
            $clientSync.Contains("DIGEST_MISMATCH")) {
            throw "A stable headless pair reported a gameplay digest mismatch."
        }
        if ($hostSync.Contains("DESYNC_ABORT") -or
            $clientSync.Contains("DESYNC_ABORT")) {
            throw "A stable headless pair aborted because its simulation diverged."
        }
        $hostEpochs = [regex]::Matches($hostSync, "GAMEPLAY_EPOCH_SYNC frame=(\d+).*checksum=(\d+)")
        $clientEpochs = [regex]::Matches($clientSync, "GAMEPLAY_EPOCH_SYNC frame=(\d+).*checksum=(\d+)")
        $hostEpochSequence = $hostEpochs | ForEach-Object {
            "$($_.Groups[1].Value):$($_.Groups[2].Value)"
        }
        $clientEpochSequence = $clientEpochs | ForEach-Object {
            "$($_.Groups[1].Value):$($_.Groups[2].Value)"
        }
        if ($hostEpochs.Count -eq 0 -or
            ($hostEpochSequence -join "|") -ne ($clientEpochSequence -join "|")) {
            throw "Headless peers did not establish identical gameplay epochs."
        }
        foreach ($syncLog in @($hostSync, $clientSync)) {
            $epochMatches = [regex]::Matches($syncLog, "GAMEPLAY_EPOCH_SYNC frame=(\d+)")
            $frameMatches = [regex]::Matches($syncLog, "FRAME_INPUT frame=(\d+)")
            $epochFrame = [int]$epochMatches[$epochMatches.Count - 1].Groups[1].Value
            $latestFrame = if ($frameMatches.Count) {
                [int]$frameMatches[$frameMatches.Count - 1].Groups[1].Value
            } else { -1 }
            if ($latestFrame -lt $epochFrame + 30) {
                throw "A peer synchronized its gameplay epoch but did not advance 30 post-rebase frames."
            }
        }
        $samplePattern = "SIM_FRAME frame=(\d+) gameplay_checksum=(\d+)"
        $hostSamples = [regex]::Matches($hostSync, $samplePattern) |
            ForEach-Object { "$($_.Groups[1].Value):$($_.Groups[2].Value)" }
        $clientSamples = [regex]::Matches($clientSync, $samplePattern) |
            ForEach-Object { "$($_.Groups[1].Value):$($_.Groups[2].Value)" }
        if ($hostSamples.Count -eq 0 -or
            ($hostSamples -join "|") -ne ($clientSamples -join "|")) {
            throw "Headless peers did not report identical sampled gameplay states."
        }
        $results.Add([pscustomobject]@{
            run = $gameRun
            scenario = if ($PeerLossRun -gt 0 -and $gameRun -gt $PeerLossRun) {
                "post-fault-recovery"
            } else { "stable" }
            passed = $true
            epochs = $hostEpochs.Count
            last_sample = $hostSamples[-1]
            evidence = $evidence
        })
        Write-Host "Headless host/join cold boot $gameRun/$GameRuns passed; evidence: $evidence"
    }
    finally {
        Remove-Item Env:BTGA_DATA_DIR -ErrorAction SilentlyContinue
        Remove-Item Env:BTGA_ONLINE_LOCAL_VIEW -ErrorAction SilentlyContinue
        Remove-Item Env:BTGA_HEADLESS_VISIBLE -ErrorAction SilentlyContinue
        Remove-Item Env:BTGA_REPLAY_EQUIVALENCE_PROBE -ErrorAction SilentlyContinue
        foreach ($process in @($hostProcess, $clientProcess)) {
            if ($null -ne $process -and !$process.HasExited) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
        }
    }
    }
    $report = Join-Path $evidenceRoot "summary.json"
    $sourceRevision = (& git -C $root rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) { $sourceRevision = "unknown" }
    $summary = [pscustomobject]@{
        generated_utc = [DateTime]::UtcNow.ToString("o")
        source_revision = ($sourceRevision | Select-Object -First 1).Trim()
        runtime_path = $runtime
        runtime_sha256 = (Get-FileHash -Algorithm SHA256 $runtime).Hash.ToLowerInvariant()
        relay_runs = $RelayRuns
        game_runs = $GameRuns
        peer_loss_run = $PeerLossRun
        replay_equivalence_probe = [bool]$ReplayEquivalenceProbe
        results = $results
    }
    $summary | ConvertTo-Json -Depth 5 | Set-Content -Path $report -Encoding UTF8
    Write-Host "Headless multiplayer report: $report"
}
