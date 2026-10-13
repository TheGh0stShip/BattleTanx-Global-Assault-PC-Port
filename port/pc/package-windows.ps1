[CmdletBinding()]
param(
    [string] $BuildDirectory = "build-pc-windows",
    [string] $OutputDirectory = "dist",
    [string] $Version = "dev",
    [switch] $Portable
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $root $BuildDirectory
}
$output = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory
} else {
    Join-Path $root $OutputDirectory
}

$packageName = "BattleTanxGA-$Version-windows-x64" + $(if ($Portable) { "-portable" } else { "" })
$staging = Join-Path $output $packageName
$archive = Join-Path $output "$packageName.zip"
$required = @(
    (Join-Path $build "BattleTanxGA.exe"),
    (Join-Path $build "SDL2.dll"),
    (Join-Path $build "dxcompiler.dll"),
    (Join-Path $build "dxil.dll"),
    (Join-Path $root "port\pc\launch-windows.cmd"),
    (Join-Path $root "port\pc\scripts\run-dedicated-server.ps1"),
    (Join-Path $root "port\pc\scripts\run-dedicated-server.cmd"),
    (Join-Path $root "COPYING"),
    (Join-Path $root "port\pc\README.md")
)

foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required package input is missing: $file"
    }
}

New-Item -ItemType Directory -Path $output -Force | Out-Null
if (Test-Path -LiteralPath $staging) {
    Remove-Item -LiteralPath $staging -Recurse -Force
}
New-Item -ItemType Directory -Path $staging | Out-Null

Copy-Item (Join-Path $build "BattleTanxGA.exe") $staging
Copy-Item (Join-Path $build "SDL2.dll") $staging
Copy-Item (Join-Path $build "dxcompiler.dll") $staging
Copy-Item (Join-Path $build "dxil.dll") $staging
Copy-Item (Join-Path $root "port\pc\launch-windows.cmd") (Join-Path $staging "Launch BattleTanx GA.cmd")
Copy-Item (Join-Path $root "port\pc\scripts\run-dedicated-server.ps1") $staging
Copy-Item (Join-Path $root "port\pc\scripts\run-dedicated-server.cmd") `
    (Join-Path $staging "Run Dedicated Server.cmd")
Copy-Item (Join-Path $root "COPYING") (Join-Path $staging "COPYING.txt")
Copy-Item (Join-Path $root "port\pc\README.md") (Join-Path $staging "README.md")
if ($Portable) {
    # The runtime treats this marker as an opt-in request to keep every
    # writable file beside the executable under userdata\.
    Set-Content -LiteralPath (Join-Path $staging "portable.txt") -Value "BattleTanxGA portable mode" -NoNewline
}

if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -LiteralPath $staging -DestinationPath $archive -CompressionLevel Optimal
Write-Host "Packaged $archive"
