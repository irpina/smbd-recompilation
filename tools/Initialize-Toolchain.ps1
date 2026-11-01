$ErrorActionPreference = 'Stop'
$clang = (Get-Command clang-cl.exe -CommandType Application -ErrorAction SilentlyContinue).Source
if (!$clang -and (Test-Path -LiteralPath "$env:ProgramFiles\LLVM\bin\clang-cl.exe")) {
    $clang = "$env:ProgramFiles\LLVM\bin\clang-cl.exe"
}
if (!$clang) { throw 'Install LLVM 22.1 or newer, including clang-cl and LLD.' }
$version = @(& $clang --version)[0]
if ($version -notmatch 'clang version (\d+\.\d+\.\d+)' -or [version]$Matches[1] -lt [version]'22.1.0') {
    throw 'LLVM 22.1 or newer is required.'
}
$llvmDirectory = Split-Path -Parent $clang
foreach ($name in @('lld-link.exe', 'llvm-lib.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $llvmDirectory $name))) { throw "LLVM tool is missing: $name" }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2026 C++ tools and the Windows SDK.' }
$visualStudio = & $vswhere -latest -products '*' -version '[18.0,19.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw 'Visual Studio 2026 C++ tools and the Windows SDK are required.' }
if(([string]$env:VSINSTALLDIR).TrimEnd('\') -ne $visualStudio.TrimEnd('\') -or
   $env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -ne 'x64' -or !$env:INCLUDE -or !$env:LIB) {
    # Reuse a matching environment: repeated batched builds otherwise grow the
    # dev-shell command line until Windows rejects it as too long.
    & (Join-Path $visualStudio 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation -NoLogo
}
$ninjaDirectory = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$toolDirectories = @($llvmDirectory,$ninjaDirectory)
$pathRemainder = @($env:Path -split ';' | Where-Object { $_.TrimEnd('\') -notin $toolDirectories })
$env:Path = ($toolDirectories + $pathRemainder) -join ';'
foreach ($name in @('cmake.exe', 'git.exe', 'ninja.exe', 'ml64.exe', 'rc.exe')) {
    if (!(Get-Command $name -CommandType Application -ErrorAction SilentlyContinue)) { throw "Build tool is missing: $name" }
}
