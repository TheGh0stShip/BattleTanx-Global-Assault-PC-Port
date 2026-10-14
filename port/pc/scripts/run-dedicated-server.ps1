[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Rom,
    [string] $BuildDirectory = "build-pc-windows",
    [string] $StateDirectory = "",
    [ValidateRange(1, 100)]
    [int] $MaximumConsecutiveFailures = 10,
    [ValidateRange(10, 3600)]
    [int] $StableRunSeconds = 300,
    [ValidateRange(1, 600)]
    [int] $HangTimeoutSeconds = 60,
    [ValidateRange(1, 900)]
    [int] $StartupHealthSeconds = 120
)

$ErrorActionPreference = "Stop"
function Get-AuthorityRuntimeFingerprint {
    param([string] $Executable, [string] $Directory)
    $files = @((Get-Item -LiteralPath $Executable)) +
        @(Get-ChildItem -LiteralPath $Directory -Filter "*.dll" -File)
    return (($files | Sort-Object -Property Name | ForEach-Object {
        "$($_.Name):$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)"
    }) -join ";")
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$packagedServer = Join-Path $PSScriptRoot "BattleTanxGA.exe"
$packaged = Test-Path -LiteralPath $packagedServer -PathType Leaf
$root = if ($packaged) { $PSScriptRoot } else { $repositoryRoot }
$build = if ($packaged -and $BuildDirectory -eq "build-pc-windows") {
    $PSScriptRoot
} elseif ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $repositoryRoot $BuildDirectory
}
$romPath = (Resolve-Path -LiteralPath $Rom).Path
$server = Join-Path $build "BattleTanxGA.exe"
if (-not (Test-Path -LiteralPath $server -PathType Leaf)) {
    throw "BattleTanxGA.exe was not found in $build. Build the Windows port first."
}
if ([IO.Path]::GetExtension($romPath).ToLowerInvariant() -ne ".z64") {
    throw "The dedicated server requires a local .z64 ROM."
}
$server = (Resolve-Path -LiteralPath $server).Path
$runtimeFingerprint = Get-AuthorityRuntimeFingerprint $server $build
$romHash = (Get-FileHash -LiteralPath $romPath -Algorithm SHA256).Hash

$state = if ($StateDirectory) {
    if ([IO.Path]::IsPathRooted($StateDirectory)) {
        $StateDirectory
    } else {
        Join-Path $root $StateDirectory
    }
} else {
    if ($packaged) {
        Join-Path $PSScriptRoot "userdata\dedicated-state"
    } else {
        Join-Path $build "dedicated-state"
    }
}
$logs = Join-Path $state "logs"
$healthPath = Join-Path $state "authority-health.json"
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$lockPath = Join-Path $state "supervisor.lock"
$stateLock = $null
try {
    $stateLock = [IO.File]::Open(
        $lockPath, [IO.FileMode]::OpenOrCreate,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
} catch {
    throw "Another dedicated authority supervisor owns $state. Stop it before starting a second copy."
}
$identityPath = Join-Path $state "runtime-identity.json"
$identity = [ordered]@{
    version = 1
    runtime = $runtimeFingerprint
    rom = $romHash
} | ConvertTo-Json -Compress
if (Test-Path -LiteralPath $identityPath -PathType Leaf) {
    $storedIdentity = (Get-Content -LiteralPath $identityPath -Raw).Trim()
    if ($storedIdentity -ne $identity) {
        $stateLock.Dispose()
        $stateLock = $null
        throw "Authority state belongs to a different executable, DLL set, or ROM. Use a fresh state directory."
    }
} else {
    $identityTemporary = "$identityPath.tmp-$PID"
    Set-Content -LiteralPath $identityTemporary -Value $identity -NoNewline
    Move-Item -LiteralPath $identityTemporary -Destination $identityPath
}

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
        $currentRuntimeFingerprint = Get-AuthorityRuntimeFingerprint $server $build
        $currentRomHash = (Get-FileHash -LiteralPath $romPath -Algorithm SHA256).Hash
        if ($currentRuntimeFingerprint -ne $runtimeFingerprint) {
            throw "Authority executable or DLL set changed while the supervisor was active; refusing to mix builds in one persistent session."
        }
        if ($currentRomHash -ne $romHash) {
            throw "Authority ROM changed while the supervisor was active; refusing to mix ROMs in one persistent session."
        }
        ++$generation
        $stamp = [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssZ")
        $prefix = "authority-$stamp-$generation"
        $stdout = Join-Path $logs "$prefix.stdout.log"
        $stderr = Join-Path $logs "$prefix.stderr.log"
        $started = [DateTime]::UtcNow
        $healthObserved = $false
        $lastHealthWrite = [DateTime]::MinValue
        $hung = $false
        $child = Start-Process $server -PassThru -WindowStyle Minimized `
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
            $now = [DateTime]::UtcNow
            if (Test-Path -LiteralPath $healthPath -PathType Leaf) {
                $healthWrite = (Get-Item -LiteralPath $healthPath).LastWriteTimeUtc
                if ($healthWrite -ge $started) {
                    $healthObserved = $true
                    $lastHealthWrite = $healthWrite
                }
            }
            if ($healthObserved -and
                ($now - $lastHealthWrite).TotalSeconds -gt $HangTimeoutSeconds) {
                $hung = $true
            }
            if (-not $healthObserved -and
                ($now - $started).TotalSeconds -gt $StartupHealthSeconds) {
                $hung = $true
            }
            if ($hung) {
                Write-Warning "Authority generation $generation stopped advancing its main loop; terminating it for recovery."
                Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue
                break
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
    if ($stateLock) { $stateLock.Dispose() }
}
