#Requires -Version 5.1
<#
    Nova verify loop
    ----------------
    Fast per-edit gate: set up the MSVC environment, configure (first run only),
    do an incremental Ninja build, then run the tests via CTest. The tests include
    the Phase 0 determinism gate (identical world hash at 1, 4 and 20 threads) and
    the no-STL include check.

    Exit code 0  => build + all tests green.
    Exit code !0 => configure / build / test failure.

    Adapted from itracker's .claude/verify.ps1. clang-tidy and release builds
    belong in CI, not here -- this stays lean.
#>

$ErrorActionPreference = 'Stop'
$sw = [System.Diagnostics.Stopwatch]::StartNew()

# Repo root is the parent of this .claude/ directory.
$repo  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build'

function Fail($msg, $code) {
    Write-Host "verify: FAIL - $msg" -ForegroundColor Red
    exit $code
}

# --- locate the Visual Studio C++ build tools (version-agnostic) ----------
# vswhere ships in a fixed, version-independent location. -latest takes whatever
# VS is newest, -products * also accepts the standalone Build Tools SKU, and
# -requires ...VC.Tools.x86.x64 demands the MSVC C++ compiler.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'vswhere.exe not found (install Visual Studio or the VS Build Tools with the C++ workload).' 10 }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail 'No Visual Studio with the C++ build tools (cl.exe) found.' 10 }

# --- enter the MSVC dev environment (only if not already loaded) ----------
# Ninja + cl need the compiler, Windows SDK and INCLUDE/LIB on the environment.
if (-not $env:VSCMD_VER) {
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { Fail "vcvars64.bat not found at $vcvars" 10 }
    & cmd /c "call `"$vcvars`" >NUL 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2])
        }
    }
}

# --- resolve cmake / ninja (prefer PATH, fall back to VS-bundled copies) ---
function Resolve-Tool($name, $fallback) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if (Test-Path $fallback) { return $fallback }
    Fail "$name not found (looked on PATH and at $fallback)" 11
}
$cmake = Resolve-Tool 'cmake' (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
$ninja = Resolve-Tool 'ninja' (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe')

# --- configure (first run / after a clean only) ---------------------------
if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
    Write-Host 'verify: configuring (Ninja, Debug)...' -ForegroundColor Cyan
    & $cmake -S $repo -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug
    if ($LASTEXITCODE -ne 0) { Fail "configure failed ($LASTEXITCODE)" $LASTEXITCODE }
}

# --- incremental build ----------------------------------------------------
& $cmake --build $build
if ($LASTEXITCODE -ne 0) { Fail "build failed ($LASTEXITCODE)" $LASTEXITCODE }

# --- tests ----------------------------------------------------------------
# ctest lives alongside cmake (neither is necessarily on PATH), so resolve it explicitly.
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
& $ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { Fail "tests failed ($LASTEXITCODE)" $LASTEXITCODE }

# --- clang-format (advisory only for now; CI will enforce) ----------------
$fmt = Join-Path $vs 'VC\Tools\Llvm\bin\clang-format.exe'
if (Test-Path $fmt) {
    # clang-format --Werror writes to stderr and returns nonzero on drift. Under
    # ErrorActionPreference='Stop' that stderr is wrapped as a terminating error
    # in PS 5.1 and would abort the loop, so relax it locally and swallow every
    # stream; we read only the exit code. This step is advisory: it never fails.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $sources = Get-ChildItem -Path (Join-Path $repo 'core'), (Join-Path $repo 'app'), (Join-Path $repo 'tests') `
        -Recurse -Include *.cpp, *.h, *.hpp -ErrorAction SilentlyContinue
    $drift = @()
    foreach ($s in $sources) {
        & $fmt --dry-run --Werror $s.FullName *> $null
        if ($LASTEXITCODE -ne 0) { $drift += $s.Name }
    }
    $ErrorActionPreference = $prevEAP
    if ($drift.Count -gt 0) {
        Write-Host "verify: clang-format drift (advisory): $($drift -join ', ')" -ForegroundColor Yellow
    }
} else {
    # Say so rather than skip silently: the newest VS may lack the Clang component.
    Write-Host "verify: clang-format not found in $vs (advisory step skipped)" -ForegroundColor DarkYellow
}

$sw.Stop()
Write-Host ("verify: OK - green in {0:N0} ms" -f $sw.Elapsed.TotalMilliseconds) -ForegroundColor Green
exit 0
