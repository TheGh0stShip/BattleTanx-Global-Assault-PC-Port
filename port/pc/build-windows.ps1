[CmdletBinding()]
param(
    [string] $BuildDirectory = "build-pc-windows",
    [string] $Rom,
    [switch] $Run
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ($root.StartsWith("\\")) {
    throw "Build from a native Windows checkout (for example C:\src\BattleTanx-Global-Assault-PC-Port), not a WSL UNC path."
}

$runtime = Join-Path $root ".deps\N64ModernRuntime"
$rt64 = Join-Path $root ".deps\rt64"
$recompRoot = Join-Path $root ".deps\N64Recomp"
$recompExe = Join-Path $recompRoot "build\N64Recomp.exe"
$recompDirectory = Join-Path $root "port\pc\recomp"
$recompInput = Join-Path $recompDirectory "input"
$generatedDirectory = Join-Path $recompDirectory "generated"
$generated = Join-Path $root "port\pc\recomp\generated\functions\funcs.h"
$recompConfig = Join-Path $root "port\pc\recomp\btga.toml"
$recompFingerprint = Join-Path $root "port\pc\recomp\generated\btga.toml.sha256"
foreach ($required in @(
    (Join-Path $runtime "CMakeLists.txt"),
    (Join-Path $rt64 "CMakeLists.txt"),
    $recompExe,
    (Join-Path $recompInput "functions.toml"),
    (Join-Path $recompInput "battletanx_ga.z64"),
    (Join-Path $root "port\pc\recomp\generated\n_aspMain.cpp")
)) {
    if (-not (Test-Path $required)) {
        throw "Required generated/dependency file is missing: $required. See port/pc/recomp/README.md."
    }
}

# Generated CPU translation is deliberately rebuilt on every native runtime
# build. N64Recomp output is ignored by Git, and treating an old generated tree
# as an implicit input has repeatedly produced binaries that did not contain
# the current hooks. Keep translation in the build flow instead of relying on
# timestamps, copied worktrees, or a developer remembering a separate step.
Remove-Item (Join-Path $generatedDirectory "functions") -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $generatedDirectory "battletanx_ga.bin") -Force -ErrorAction SilentlyContinue
Push-Location $recompDirectory
try {
    & $recompExe "btga.toml"
    if ($LASTEXITCODE -ne 0) { throw "N64Recomp translation failed." }
} finally {
    Pop-Location
}
if (-not (Test-Path $generated)) {
    throw "N64Recomp completed without producing $generated."
}
$expectedRecompHash = (Get-FileHash -Algorithm SHA256 $recompConfig).Hash.ToLowerInvariant()
Set-Content -NoNewline -Encoding ascii $recompFingerprint $expectedRecompHash

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "Visual Studio Installer's vswhere.exe was not found. Install Visual Studio with Desktop development with C++."
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    throw "A Visual Studio installation with the x64 C++ toolchain was not found."
}

$devShell = Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
$ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$source = Join-Path $root "port\pc"
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $root $BuildDirectory
}
$sdl = Join-Path $rt64 "src\contrib\mupen64plus-win32-deps\SDL2-2.26.3"

& $cmake -S $source -B $build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
    -DCMAKE_BUILD_TYPE=Release `
    -DBTGA_BUILD_RECOMP=ON `
    -DBTGA_BUILD_RUNTIME_PROBE=ON `
    -DBTGA_BUILD_RT64=ON `
    "-DBTGA_N64_RUNTIME_ROOT=$runtime" `
    "-DBTGA_RT64_ROOT=$rt64" `
    "-DSDL2_DIR=$(Join-Path $sdl 'cmake')"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

& $cmake --build $build --target btga_runtime_probe btga_pc_tests `
    btga_pc_synchronization_soak btga_authoritative_server_soak `
    btga_canonical_journal_soak `
    btga_dedicated_client_bot --parallel
if ($LASTEXITCODE -ne 0) { throw "Native build failed." }

& $ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Native tests failed." }

Copy-Item (Join-Path $sdl "lib\x64\SDL2.dll") $build -Force
Copy-Item (Join-Path $rt64 "src\contrib\dxc\bin\x64\dxcompiler.dll") $build -Force
Copy-Item (Join-Path $rt64 "src\contrib\dxc\bin\x64\dxil.dll") $build -Force

$exe = Join-Path $build "BattleTanxGA.exe"
Write-Host "Built $exe"
if ($Run) {
    if (-not $Rom) { throw "-Run requires -Rom with a local USA Rev 0 .z64 file." }
    $resolvedRom = (Resolve-Path $Rom).Path
    Start-Process -FilePath $exe -ArgumentList @("--play", $resolvedRom) -WorkingDirectory $build
}
