[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Rom,
    [string] $BuildDirectory = "build-pc-windows"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $root $BuildDirectory
}
$romPath = (Resolve-Path $Rom).Path
$game = Join-Path $build "BattleTanxGA.exe"
$bot = Join-Path $build "btga_dedicated_client_bot.exe"
if (-not (Test-Path $game) -or -not (Test-Path $bot)) {
    throw "Build BattleTanxGA.exe and btga_dedicated_client_bot.exe first."
}

$evidence = Join-Path $build "dedicated-play"
New-Item $evidence -ItemType Directory -Force | Out-Null
Get-ChildItem $evidence -File -ErrorAction SilentlyContinue | Remove-Item -Force
$processes = [System.Collections.Generic.List[Diagnostics.Process]]::new()
try {
    $authorityOut = Join-Path $evidence "authority.stdout.log"
    $authorityErr = Join-Path $evidence "authority.stderr.log"
    $authority = Start-Process $game -PassThru `
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

    $clientOut = Join-Path $evidence "client.stdout.log"
    $clientErr = Join-Path $evidence "client.stderr.log"
    $client = Start-Process $game -PassThru `
        -ArgumentList @("--dedicated-join", $lobby, $romPath) `
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
    } until ($clientText.Contains("BTGA_CLIENT bound authoritative seat=0") -or
             $client.HasExited -or [DateTime]::UtcNow -ge $deadline)
    if (-not $clientText.Contains("BTGA_CLIENT bound authoritative seat=0")) {
        throw "Rendered client did not bind dedicated seat 0."
    }

    for ($seat = 1; $seat -lt 4; ++$seat) {
        $out = Join-Path $evidence "bot-$seat.stdout.log"
        $err = Join-Path $evidence "bot-$seat.stderr.log"
        $peer = Start-Process $bot -PassThru `
            -ArgumentList @("--join", $lobby, $seat, 300) `
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
        } until ($peerText.Contains("BOUND_SEAT=$seat") -or
                 $peer.HasExited -or [DateTime]::UtcNow -ge $deadline)
        if (-not $peerText.Contains("BOUND_SEAT=$seat")) {
            throw "Automated seat $seat did not bind."
        }
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 100
        $authorityText = ""
        if (Test-Path $authorityErr) {
            $content = Get-Content $authorityErr -Raw
            if ($null -ne $content) { $authorityText = [string]$content }
        }
    } until ($authorityText.Contains("BTGA_AUTHORITY all 4 seats ready") -or
             $authority.HasExited -or [DateTime]::UtcNow -ge $deadline)
    if (-not $authorityText.Contains("BTGA_AUTHORITY all 4 seats ready")) {
        throw "Dedicated authority did not receive readiness from all four seats."
    }

    Write-Host "Dedicated four-player session ready. Lobby: $lobby"
    Write-Host "Close the BattleTanxGA window to stop the authority and test peers."
    Write-Host "Logs: $evidence"
    $client.WaitForExit()
} finally {
    foreach ($process in $processes) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit(5000) | Out-Null
        }
    }
}
