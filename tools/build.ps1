#Requires -Version 5.1
<#
    Nova: build nova.exe (Release) into build-release\bin
    -----------------------------------------------------
    The folder the README's commands and tools\abio.ps1 use. Configures on the
    first run only; after that it's an incremental Ninja build. The tests
    aren't built here (.claude\verify.ps1 builds and runs them).

      tools\build.ps1
#>
$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build-release'

function Fail($msg, $code) {
    Write-Host "build: FAIL - $msg" -ForegroundColor Red
    exit $code
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'vswhere.exe not found (is Visual Studio installed?)' 10 }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail 'No Visual Studio with the C++ build tools found.' 10 }
if (-not $env:VSCMD_VER) {
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    & cmd /c "call `"$vcvars`" >NUL 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
}
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'

if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
    & $cmake -S $repo -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release -DNOVA_BUILD_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { Fail "configure failed ($LASTEXITCODE)" $LASTEXITCODE }
}
& $cmake --build $build --target nova
if ($LASTEXITCODE -ne 0) { Fail "build failed ($LASTEXITCODE)" $LASTEXITCODE }
Write-Host "build: OK - $(Join-Path $build 'bin\nova.exe')" -ForegroundColor Green
