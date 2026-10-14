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
    [ValidateRange(2, 1000)]
    [int] $DropOutgoingEvery = 10,
    [ValidateRange(2, 1000)]
    [int] $DropIncomingEvery = 10,
    [ValidateRange(1024, 65532)]
    [int] $FirstRelayPort = 30421
)

$ErrorActionPreference = "Stop"
$test = Join-Path $PSScriptRoot "test-local-dedicated-multiplayer.ps1"
$relayRestartTest = Join-Path $PSScriptRoot "test-dedicated-relay-restart.ps1"
if (-not (Test-Path $test -PathType Leaf)) {
    throw "Dedicated multiplayer test script is missing: $test"
}
if (-not (Test-Path $relayRestartTest -PathType Leaf)) {
    throw "Dedicated relay-restart test script is missing: $relayRestartTest"
}

$common = @{
    Rom = $Rom
    NetworkingRoot = $NetworkingRoot
    FixturePath = $FixturePath
    ExternalRelay = $ExternalRelay
    NodeExecutable = $NodeExecutable
    BuildDirectory = $BuildDirectory
    Seconds = $Seconds
    Players = 4
    DropOutgoingEvery = $DropOutgoingEvery
    DropIncomingEvery = $DropIncomingEvery
}
$cases = @(
    @{
        Name = "interrupted-initial-bootstrap"
        Arguments = @{ InterruptInitialBootstrap = $true }
    },
    @{
        Name = "authority-restart-plus-all-seats"
        Arguments = @{
            ColdJoinAfterAuthorityRestart = $true
            AllSeatReconnectCycles = 2
        }
    },
    @{
        Name = "three-all-seat-sweeps"
        Arguments = @{ AllSeatReconnectCycles = 3 }
    },
    @{
        Name = "automatic-client-resynchronization"
        Arguments = @{ InjectClientDesyncTick = 120 }
    }
)

$root = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    (Resolve-Path $BuildDirectory).Path
} else {
    (Resolve-Path (Join-Path $root $BuildDirectory)).Path
}
$evidenceRoot = Join-Path $build "local-dedicated-evidence"
$matrixStarted = [DateTime]::UtcNow
$results = [System.Collections.Generic.List[object]]::new()
$totalCases = $cases.Count + $(if ($ExternalRelay) { 0 } else { 1 })

for ($index = 0; $index -lt $cases.Count; $index++) {
    $case = $cases[$index]
    $caseArguments = $case.Arguments
    $relayPort = $FirstRelayPort + $index
    Write-Host ("[{0}/{1}] Dedicated recovery gate: {2}" -f `
        ($index + 1), $totalCases, $case.Name)
    $existingEvidence = @{}
    if (Test-Path $evidenceRoot -PathType Container) {
        Get-ChildItem $evidenceRoot -Directory | ForEach-Object {
            $existingEvidence[$_.FullName] = $true
        }
    }
    $started = [DateTime]::UtcNow
    & $test @common @caseArguments -RelayPort $relayPort
    $finished = [DateTime]::UtcNow
    $evidence = Get-ChildItem $evidenceRoot -Directory |
        Where-Object { -not $existingEvidence.ContainsKey($_.FullName) } |
        Sort-Object CreationTimeUtc -Descending |
        Select-Object -First 1
    if ($null -eq $evidence) {
        throw "Gate '$($case.Name)' passed without producing an evidence directory."
    }
    $results.Add([ordered]@{
        gate = $case.Name
        relay_port = $relayPort
        started_utc = $started.ToString("o")
        finished_utc = $finished.ToString("o")
        duration_seconds = [Math]::Round(($finished - $started).TotalSeconds, 3)
        evidence = $evidence.FullName
        passed = $true
    })
}

if (-not $ExternalRelay) {
    Write-Host ("[{0}/{1}] Dedicated recovery gate: complete-relay-restart" -f `
        ($cases.Count + 1), $totalCases)
    $relayRestartStarted = [DateTime]::UtcNow
    & $relayRestartTest -Rom $Rom -NetworkingRoot $NetworkingRoot `
        -NodeExecutable $NodeExecutable -BuildDirectory $BuildDirectory `
        -Seconds ([Math]::Max($Seconds, 150)) `
        -MasterPort ($FirstRelayPort + 4) -RelayPort ($FirstRelayPort + 5)
    $relayRestartFinished = [DateTime]::UtcNow
    $restartEvidenceRoot = Join-Path $build "relay-restart-evidence"
    $restartEvidence = Get-ChildItem $restartEvidenceRoot -Directory |
        Sort-Object CreationTimeUtc -Descending | Select-Object -First 1
    if ($null -eq $restartEvidence -or
        -not (Test-Path (Join-Path $restartEvidence.FullName "result.json"))) {
        throw "Relay restart gate passed without producing evidence."
    }
    $results.Add([ordered]@{
        gate = "complete-relay-restart"
        relay_port = $FirstRelayPort + 5
        started_utc = $relayRestartStarted.ToString("o")
        finished_utc = $relayRestartFinished.ToString("o")
        duration_seconds = [Math]::Round(
            ($relayRestartFinished - $relayRestartStarted).TotalSeconds, 3)
        evidence = $restartEvidence.FullName
        passed = $true
    })
}

$matrixFinished = [DateTime]::UtcNow
$summary = [ordered]@{
    schema = 1
    started_utc = $matrixStarted.ToString("o")
    finished_utc = $matrixFinished.ToString("o")
    duration_seconds = [Math]::Round(
        ($matrixFinished - $matrixStarted).TotalSeconds, 3)
    players = 4
    gates = $results
    passed = $true
}
$summaryPath = Join-Path $evidenceRoot (
    "recovery-matrix-{0}.json" -f $matrixStarted.ToString("yyyyMMddTHHmmssfffZ"))
$summary | ConvertTo-Json -Depth 5 | Set-Content $summaryPath -Encoding UTF8
Write-Host "Dedicated recovery matrix passed: $summaryPath"
