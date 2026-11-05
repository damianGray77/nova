#Requires -Version 5.1
<#
    Nova benchmark (design.md 7.1 I, Phase 1a gate 3)
    -------------------------------------------------
    Builds the Release preset into build-release/ and runs `nova bench`:
    interpreter throughput on three workloads, single-threaded and on all
    threads. Kept out of verify.ps1 on purpose: timings are noisy and the
    verify loop must stay fast and deterministic.

      tools\bench.ps1                 # 3 s per measurement, all threads
      tools\bench.ps1 -Seconds 10 -Threads 14
#>
param(
    [double]$Seconds = 3,
    [int]$Threads = 0
)

$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build-release'

function Fail($msg, $code) {
    Write-Host "bench: FAIL - $msg" -ForegroundColor Red
    exit $code
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'vswhere.exe not found.' 10 }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail 'No Visual Studio with the C++ build tools found.' 10 }
if (-not $env:VSCMD_VER) {
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    & cmd /c "call `"$vcvars`" >NUL 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
}
function Resolve-Tool($name, $fallback) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if (Test-Path $fallback) { return $fallback }
    Fail "$name not found" 11
}
$cmake = Resolve-Tool 'cmake' (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
$ninja = Resolve-Tool 'ninja' (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe')

if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
    & $cmake -S $repo -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release -DNOVA_BUILD_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { Fail "configure failed ($LASTEXITCODE)" $LASTEXITCODE }
}
& $cmake --build $build --target nova
if ($LASTEXITCODE -ne 0) { Fail "build failed ($LASTEXITCODE)" $LASTEXITCODE }

$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
Write-Host "bench: $cpu"
& (Join-Path $build 'bin\nova.exe') bench $Seconds --threads $Threads
exit $LASTEXITCODE
