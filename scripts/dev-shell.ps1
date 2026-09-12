<#
.SYNOPSIS
    Puts the current PowerShell session into a Visual Studio developer
    environment with CMake, Ninja, the LLVM tools and vcpkg on PATH.

.DESCRIPTION
    CI gets this from `ilammy/msvc-dev-cmd` plus explicit tool installs; this
    script is the local equivalent, so `cmake --preset ...` behaves the same on a
    developer box as it does on a runner.

    Dot-source it so the environment sticks:

        . .\scripts\dev-shell.ps1
        cmake --preset x64-debug

.PARAMETER Architecture
    Target architecture for the MSVC toolset: x64 (default) or arm64.

.PARAMETER VcpkgRoot
    Where vcpkg lives. Defaults to $env:VCPKG_ROOT, then to a `vcpkg` checkout
    next to this repository. Visual Studio ships a cut-down vcpkg that has no
    ports tree, so it is never used even when the VS environment points at it.
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')]
    [string]$Architecture = 'x64',

    [string]$VcpkgRoot
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-VsInstallPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found; install Visual Studio 2022 17.10+ with the C++ workload."
    }
    # -prerelease so an Insiders/Preview install counts: it is a normal C++
    # toolset, but vswhere hides prerelease channels unless asked for them, and
    # without this the script refuses on a machine that can build perfectly well.
    $path = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path) {
        throw "No Visual Studio installation with the C++ toolset was found."
    }
    return $path
}

function Resolve-VcpkgRoot {
    param([string]$Requested, [string]$RepoRoot)

    $candidates = @(
        $Requested,
        $env:VCPKG_ROOT,
        (Join-Path (Split-Path -Parent $RepoRoot) 'vcpkg'),
        (Join-Path $env:USERPROFILE 'vcpkg')
    ) | Where-Object { $_ }

    foreach ($candidate in $candidates) {
        # A usable vcpkg has a ports tree; the one bundled with Visual Studio does not.
        if ((Test-Path (Join-Path $candidate 'vcpkg.exe')) -and (Test-Path (Join-Path $candidate 'ports'))) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw @"
No usable vcpkg checkout found. Clone and bootstrap one, then re-run:

    git clone https://github.com/microsoft/vcpkg $(Join-Path (Split-Path -Parent $RepoRoot) 'vcpkg')
    & $(Join-Path (Split-Path -Parent $RepoRoot) 'vcpkg\bootstrap-vcpkg.bat')
"@
}

$vsInstallPath = Resolve-VsInstallPath
$resolvedVcpkg = Resolve-VcpkgRoot -Requested $VcpkgRoot -RepoRoot $repoRoot

# Enter-VsDevShell shells out to vswhere, so the Installer directory has to be
# reachable before it runs.
$env:PATH = (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer') + ';' + $env:PATH

Import-Module (Join-Path $vsInstallPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstallPath -SkipAutomaticLocation `
    -DevCmdArguments "-arch=$Architecture -host_arch=x64" | Out-Null

# Enter-VsDevShell rewrites PATH and points VCPKG_ROOT at the bundled vcpkg, so
# everything below has to run after it.
$env:VCPKG_ROOT = $resolvedVcpkg

$toolPaths = @(
    (Join-Path $vsInstallPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'),
    (Join-Path $vsInstallPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'),
    (Join-Path $vsInstallPath 'VC\Tools\Llvm\x64\bin'),
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'),
    $resolvedVcpkg
) | Where-Object { Test-Path $_ }

$env:PATH = ($toolPaths -join ';') + ';' + $env:PATH

# Enter-VsDevShell reports success even when the requested cross toolset is not
# installed; it just leaves the host compiler on PATH and drops the rest of the
# environment, which surfaces much later as "CMake was unable to find a build
# program". Fail here instead, with the fix.
$compiler = Get-Command cl -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "cl.exe is not on PATH after entering the developer shell; is the C++ workload installed?"
}
if ($compiler.Source -notmatch "\\Host[^\\]+\\$Architecture\\cl\.exe$") {
    throw @"
The $Architecture toolset is not installed: cl.exe resolved to
  $($compiler.Source)
Install it from the Visual Studio Installer ("MSVC v143 - VS 2022 C++ $Architecture build tools").
"@
}

Write-Host "Visual Studio : $vsInstallPath"
Write-Host "Architecture  : $Architecture"
Write-Host "VCPKG_ROOT    : $env:VCPKG_ROOT"
foreach ($tool in 'cmake', 'ninja', 'cl', 'clang-cl', 'clang-format', 'clang-tidy', 'llvm-cov') {
    $found = Get-Command $tool -ErrorAction SilentlyContinue
    $where = if ($found) { $found.Source } else { '(not found)' }
    Write-Host ("{0,-13} : {1}" -f $tool, $where)
}
