[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Rom,
    [string] $BuildDirectory = "build-pc-windows",
    [switch] $Run
)

$ErrorActionPreference = "Stop"
$expectedSha256 = "88268ce770ff39f2ca839848b842c87866df6c5ddaf775f180b2522c18ff6221"
$n64RecompRevision = "ffb39cdad1da5de07eaaa48bd1db4a89a7986771"
$runtimeRevision = "cdf5abbd5026fef5c364c676e4667c45e42b6863"
$rt64Revision = "23cab603c4f9f4a8b369b38e036f1aa484603878"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string] $FilePath,
        [Parameter(Mandatory = $true)][string[]] $ArgumentList,
        [Parameter(Mandatory = $true)][string] $Description
    )
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

function Checkout-Pinned {
    param(
        [Parameter(Mandatory = $true)][string] $Url,
        [Parameter(Mandatory = $true)][string] $Destination,
        [Parameter(Mandatory = $true)][string] $Revision
    )
    if ((Test-Path $Destination) -and -not (Test-Path (Join-Path $Destination ".git"))) {
        Write-Host "Replacing incomplete dependency directory $Destination"
        Remove-Item $Destination -Recurse -Force
    }
    if (-not (Test-Path (Join-Path $Destination ".git"))) {
        New-Item -ItemType Directory -Force (Split-Path $Destination -Parent) | Out-Null
        Invoke-Checked git @("clone", $Url, $Destination) "Cloning $Url"
    }
    Invoke-Checked git @("-C", $Destination, "fetch", "--depth=1", "origin", $Revision) "Fetching $Url"
    Invoke-Checked git @("-C", $Destination, "checkout", "--detach", $Revision) "Checking out $Revision"
    Invoke-Checked git @("-C", $Destination, "submodule", "update", "--init", "--recursive", "--depth=1") "Updating submodules for $Url"
    $actual = (& git -C $Destination rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $actual -ne $Revision) {
        throw "Revision mismatch in ${Destination}: expected $Revision, got $actual."
    }
}

$root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ($root.StartsWith("\\")) {
    throw "Use a native Windows checkout such as C:\src\BattleTanx-Global-Assault-PC-Port, not a WSL UNC path."
}

$romPath = (Resolve-Path $Rom).Path
if ([IO.Path]::GetExtension($romPath).ToLowerInvariant() -ne ".z64") {
    throw "The input must be a local big-endian .z64 ROM dump."
}
$actualSha256 = (Get-FileHash -Algorithm SHA256 $romPath).Hash.ToLowerInvariant()
if ($actualSha256 -ne $expectedSha256) {
    throw "ROM SHA-256 mismatch. Expected $expectedSha256, got $actualSha256."
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "Git was not found. Install Git for Windows and open a new PowerShell window."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "Install Visual Studio 2022 with the Desktop development with C++ workload."
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    throw "A Visual Studio installation with the x64 C++ toolchain was not found."
}
$devShell = Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

$dependencies = Join-Path $root ".deps"
$n64Recomp = Join-Path $dependencies "N64Recomp"
$runtime = Join-Path $dependencies "N64ModernRuntime"
$rt64 = Join-Path $dependencies "rt64"

Checkout-Pinned "https://github.com/N64Recomp/N64Recomp.git" $n64Recomp $n64RecompRevision
Checkout-Pinned "https://github.com/N64Recomp/N64ModernRuntime.git" $runtime $runtimeRevision
Checkout-Pinned "https://github.com/rt64/rt64.git" $rt64 $rt64Revision

$nfd = Join-Path $rt64 "src\contrib\nativefiledialog-extended"
$patch = Join-Path $root "port\pc\recomp\patches\rt64-nfd-stub.patch"
& git -C $nfd apply --check $patch 2>$null
if ($LASTEXITCODE -eq 0) {
    Invoke-Checked git @("-C", $nfd, "apply", $patch) "Applying the RT64 file-dialog patch"
} else {
    & git -C $nfd apply --reverse --check $patch 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "The RT64 file-dialog patch does not apply cleanly."
    }
}

$recompBuild = Join-Path $n64Recomp "build"
Invoke-Checked $cmake @(
    "-S", $n64Recomp, "-B", $recompBuild, "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_BUILD_TYPE=Release"
) "Configuring N64Recomp"
Invoke-Checked $cmake @("--build", $recompBuild, "--target", "N64RecompCLI", "RSPRecomp", "--parallel") "Building N64Recomp"

$recompDirectory = Join-Path $root "port\pc\recomp"
$inputDirectory = Join-Path $recompDirectory "input"
$generatedDirectory = Join-Path $recompDirectory "generated"
New-Item -ItemType Directory -Force $inputDirectory, $generatedDirectory | Out-Null
Copy-Item (Join-Path $root "config\us\recomp_function_boundaries.toml") (Join-Path $inputDirectory "functions.toml") -Force
Copy-Item $romPath (Join-Path $inputDirectory "battletanx_ga.z64") -Force
Remove-Item (Join-Path $generatedDirectory "functions") -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $generatedDirectory "battletanx_ga.bin") -Force -ErrorAction SilentlyContinue

Push-Location $recompDirectory
try {
    Invoke-Checked (Join-Path $recompBuild "N64Recomp.exe") @("btga.toml") "Translating the game CPU code"
    Invoke-Checked (Join-Path $recompBuild "RSPRecomp.exe") @("n_aspMain.toml") "Translating the audio microcode"
} finally {
    Pop-Location
}

$buildArguments = @{
    BuildDirectory = $BuildDirectory
    Rom = $romPath
}
if ($Run) { $buildArguments.Run = $true }
& (Join-Path $root "port\pc\build-windows.ps1") @buildArguments

