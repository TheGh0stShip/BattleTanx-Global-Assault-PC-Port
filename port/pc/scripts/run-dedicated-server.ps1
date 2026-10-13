[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Rom,
    [string] $BuildDirectory = "build-pc-windows",
    [string] $StateDirectory = "",
    [ValidateRange(1, 100)]
    [int] $MaximumConsecutiveFailures = 10,
    [ValidateRange(10, 3600)]
    [int] $StableRunSeconds = 300
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $root $BuildDirectory
}
$romPath = (Resolve-Path -LiteralPath $Rom).Path
$server = Join-Path $build "BattleTanxGA.exe"
if (-not (Test-Path -LiteralPath $server -PathType Leaf)) {
    throw "BattleTanxGA.exe was not found in $build. Build the Windows port first."
}
if ([IO.Path]::GetExtension($romPath).ToLowerInvariant() -ne ".z64") {
    throw "The dedicated server requires a local .z64 ROM."
}

$state = if ($StateDirectory) {
    if ([IO.Path]::IsPathRooted($StateDirectory)) {
        $StateDirectory
    } else {
        Join-Path $root $StateDirectory
    }
} else {
    Join-Path $build "dedicated-state"
}
$logs = Join-Path $state "logs"
New-Item -ItemType Directory -Path $logs -Force | Out-Null

$previousStateDirectory = $env:BTGA_DEDICATED_STATE_DIR
$env:BTGA_DEDICATED_STATE_DIR = $state
$child = $null
$generation = 0
$failures = 0
try {
    Write-Host "Dedicated authority supervisor active."
    Write-Host "State: $state"
    Write-Host "Press Ctrl+C to stop. Existing clients may remain connected during recovery."
    while ($true) {
        ++$generation
        $stamp = [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssZ")
        $prefix = "authority-$stamp-$generation"
        $stdout = Join-Path $logs "$prefix.stdout.log"
        $stderr = Join-Path $logs "$prefix.stderr.log"
        $started = [DateTime]::UtcNow
        $child = Start-Process $server -PassThru `
            -ArgumentList @("--dedicated-server-public", $romPath) `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr

        $published = $false
        while (-not $child.HasExited) {
            if (-not $published -and (Test-Path -LiteralPath $stdout)) {
                $line = Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue |
                    Where-Object { $_ -like "ONLINE_LOBBY_ID=*" } |
                    Select-Object -First 1
                if ($line) {
                    $published = $true
                    Write-Host $line
                    Write-Host "Authority generation $generation is online."
                }
            }
            Start-Sleep -Milliseconds 250
            $child.Refresh()
        }

        $child.WaitForExit()
        $runtime = ([DateTime]::UtcNow - $started).TotalSeconds
        $exitCode = $child.ExitCode
        $child = $null
        if ($runtime -ge $StableRunSeconds) {
            $failures = 0
        }
        ++$failures
        if ($failures -gt $MaximumConsecutiveFailures) {
            throw "Dedicated authority exceeded $MaximumConsecutiveFailures consecutive failures. Last exit code: $exitCode. Logs: $stderr"
        }
        $exponent = [Math]::Min($failures - 1, 5)
        $delay = [Math]::Min(30, [Math]::Pow(2, $exponent))
        Write-Warning "Authority exited with code $exitCode after $([Math]::Round($runtime, 1)) seconds. Restarting generation $($generation + 1) in $delay second(s)."
        Start-Sleep -Seconds $delay
    }
} finally {
    if ($child -and -not $child.HasExited) {
        Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue
        $child.WaitForExit(5000) | Out-Null
    }
    $env:BTGA_DEDICATED_STATE_DIR = $previousStateDirectory
}
