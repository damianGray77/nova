#Requires -Version 5.1
<#
    Phase 1b pilot build (THROWAWAY -- see README.md).

    Copies the repo's core/ into pilots/phase1b/build/core, applies proto.patch
    (environment-variable toggles for the candidate rules), and compiles
    diag.cpp against it into build/diag.exe. The engine, its CMake build and
    verify.ps1 are untouched. build/ is git-ignored.

    The patch was made against main at 2ef4571 (Phase 1a). If core/ has moved
    on and the patch no longer applies, check out 2ef4571's core/ first:
        git -C <repo> worktree add <dir> 2ef4571
#>
$ErrorActionPreference = 'Stop'
$here  = $PSScriptRoot
$repo  = Split-Path -Parent (Split-Path -Parent $here)
$build = Join-Path $here 'build'

# MSVC environment, found the same way .claude/verify.ps1 finds it.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio with the C++ build tools found.' }
if (-not $env:VSCMD_VER) {
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    & cmd /c "call `"$vcvars`" >NUL 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
}

# Fresh patched copy of core/.
if (Test-Path (Join-Path $build 'core')) { Remove-Item -Recurse -Force (Join-Path $build 'core') }
New-Item -ItemType Directory -Force $build | Out-Null
Copy-Item -Recurse (Join-Path $repo 'core') (Join-Path $build 'core')
New-Item -ItemType Directory -Force (Join-Path $build 'obj') | Out-Null
# Apply from the repo root with --directory, so the patch's core/... paths land
# on the copy in build/ and never on the real core/. (A plain `git apply` run
# inside the repo resolves paths against the repo root instead.)
& git -C $repo apply --whitespace=nowarn --directory=pilots/phase1b/build (Join-Path $here 'proto.patch')
if ($LASTEXITCODE -ne 0) { throw 'proto.patch did not apply (see the note at the top of this script).' }
Push-Location $build
try {
    & cl /nologo /std:c++20 /O2 /MD /EHs-c- /DNDEBUG /D_CRT_SECURE_NO_WARNINGS `
        /I core\include (Join-Path $here 'diag.cpp') core\src\*.cpp /Fe:diag.exe /Fo:obj\ 2>&1 |
        Where-Object { $_ -match 'error|warning' }
    if (-not (Test-Path 'diag.exe')) { throw 'compile failed' }
} finally {
    Pop-Location
}
Write-Host "pilot: built $(Join-Path $build 'diag.exe')" -ForegroundColor Green
