<#
.SYNOPSIS
    Builds and drives the elevation/IPC spike for issue #6.

.DESCRIPTION
    Full-mode compaction (attach read-only) and direct `AttachVirtualDisk` need
    an elevated token; the common compaction path does not (D10). So the
    question is not whether to elevate, but how to elevate *half* the process
    and still give the user one output stream and one exit code.

    Five experiments, each answering something the issue asks:

        whoami   which token the two halves run with
        accept   the happy path: runas, connect, stream progress, exit code
        decline  what declining the UAC prompt does to the parent
        cancel   a real Ctrl+C in the unelevated console, delivered through
                 the parent's console with GenerateConsoleCtrlEvent
        squat    an unprivileged process holding the pipe name first, and the
                 worker's refusal to talk to a server that is not its launcher

    `accept`, `decline` and `cancel` put a UAC prompt on screen and wait for a
    human to answer it. That is the point -- the prompt is what is being
    measured. Nothing is compacted: the worker sleeps and reports progress.

.PARAMETER Experiment
    Which experiment to run. `all` runs them in the order above.

.PARAMETER Seconds
    How long the simulated elevated operation runs. The cancel experiment needs
    this long enough to interrupt.

.PARAMETER CancelAfterSeconds
    How long to wait before sending the Ctrl+C, in the cancel experiment. It has
    to cover a human answering the UAC prompt.

.PARAMETER OutDir
    Where to build. Defaults under %TEMP% so the repository stays clean.

.PARAMETER SkipBuild
    Reuse an existing elevate.exe.

.EXAMPLE
    .\run.ps1 -Experiment squat

.EXAMPLE
    .\run.ps1 -Experiment all -Seconds 6
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'whoami', 'accept', 'decline', 'cancel', 'squat')]
    [string]$Experiment = 'all',
    [int]$Seconds = 4,
    [int]$CancelAfterSeconds = 8,
    [string]$OutDir = (Join-Path $env:TEMP 'wsldisk-spike-elevation'),
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'elevate.cpp'
$exe = Join-Path $OutDir 'elevate.exe'

function Write-Banner([string]$Text) {
    Write-Host ''
    Write-Host "=== $Text " -ForegroundColor Cyan -NoNewline
    Write-Host ('=' * [Math]::Max(0, 64 - $Text.Length)) -ForegroundColor Cyan
}

function Build-Spike {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; is Visual Studio installed?" }
    $install = & $vswhere -latest -prerelease -products * -property installationPath | Select-Object -First 1
    if (-not $install) { throw 'No Visual Studio installation with C++ tools found.' }
    $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $install" }

    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    Write-Host "Building $source" -ForegroundColor DarkGray
    # No vcpkg, no WIL, no CMake: a spike should build from one command.
    $compile = '"{0}" >nul 2>&1 && cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /Fe:"{1}" /Fo:"{2}\\" "{3}" advapi32.lib shell32.lib bcrypt.lib' -f $vcvars, $exe, $OutDir, $source
    cmd /c $compile
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
}

function Invoke-Whoami {
    Write-Banner 'whoami -- which token each half runs with'
    & $exe --whoami
    Write-Host "(the worker's own line appears in the accept experiment, over the pipe)" -ForegroundColor DarkGray
}

function Invoke-Accept {
    Write-Banner 'accept -- approve the UAC prompt when it appears'
    & $exe --seconds $Seconds
    Write-Host "parent exit code: $LASTEXITCODE" -ForegroundColor Yellow
}

function Invoke-Decline {
    Write-Banner 'decline -- DENY the UAC prompt when it appears'
    & $exe --seconds $Seconds
    Write-Host "parent exit code: $LASTEXITCODE (expect 4 = NeedsElevation)" -ForegroundColor Yellow
}

function Invoke-Cancel {
    Write-Banner 'cancel -- real Ctrl+C in the unelevated console (approve the prompt)'
    $log = Join-Path $OutDir 'cancel.log'
    $err = Join-Path $OutDir 'cancel.err'
    # The worker has to still be running when the Ctrl+C lands, and the prompt
    # is answered by a human, so give the operation room past the send delay.
    $workSeconds = [Math]::Max($Seconds, $CancelAfterSeconds + 8)
    # Its own console, so GenerateConsoleCtrlEvent has something to attach to.
    $parent = Start-Process -FilePath $exe -ArgumentList '--seconds', $workSeconds -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError $err
    Write-Host "parent pid $($parent.Id); approve the prompt, then Ctrl+C is sent" -ForegroundColor DarkGray
    Start-Sleep -Seconds $CancelAfterSeconds
    & $exe --send-ctrl-c $parent.Id | Out-Null
    if (-not $parent.WaitForExit(30000)) {
        Write-Host 'parent did not exit within 30 s' -ForegroundColor Red
        $parent.Kill()
    }
    Get-Content $log -ErrorAction SilentlyContinue
    Get-Content $err -ErrorAction SilentlyContinue
    Write-Host "parent exit code: $($parent.ExitCode) (expect 5 = Partial/cancelled)" -ForegroundColor Yellow
}

function Invoke-Squat {
    Write-Banner 'squat -- an unprivileged process holds the pipe name first'
    $name = '\\.\pipe\wsldisk-squat-test'
    $squatter = Start-Process -FilePath $exe -ArgumentList '--squat', '--pipe', $name, '--seconds', 20 `
        -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 700
    try {
        Write-Host '-- 1. can a second server create the same name? (FILE_FLAG_FIRST_PIPE_INSTANCE)' -ForegroundColor DarkGray
        & $exe --squat --pipe $name --seconds 1
        Write-Host "   exit code $LASTEXITCODE" -ForegroundColor Yellow

        Write-Host '-- 2. does a worker talk to a server that did not launch it?' -ForegroundColor DarkGray
        # Unelevated on purpose: the identity check is what is under test, not the token.
        & $exe --worker --pipe $name --server-pid $PID --seconds 1
        Write-Host "   exit code $LASTEXITCODE (expect 1 = refused)" -ForegroundColor Yellow
    } finally {
        if (-not $squatter.HasExited) { $squatter.Kill() }
    }
}

if (-not $SkipBuild) { Build-Spike }
if (-not (Test-Path $exe)) { throw "$exe not found; run without -SkipBuild" }

switch ($Experiment) {
    'whoami' { Invoke-Whoami }
    'accept' { Invoke-Accept }
    'decline' { Invoke-Decline }
    'cancel' { Invoke-Cancel }
    'squat' { Invoke-Squat }
    'all' {
        Invoke-Whoami
        Invoke-Accept
        Invoke-Decline
        Invoke-Cancel
        Invoke-Squat
    }
}
