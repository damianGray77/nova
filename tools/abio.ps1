#Requires -Version 5.1
<#
    Nova abiogenesis harness (Phase 1b, design.md 6.6)
    --------------------------------------------------
    Runs paired trials -- arms x seeds -- as separate single-node processes, each
    with its own replay log, and summarises them.

      tools\abio.ps1 -Name exp1 -Hours 12 -Seeds 1,2,3 -ArmSeeds @{ baseline = 1..6 }
      tools\abio.ps1 -Name exp1 -Summarize          # rebuild summary.csv only
      tools\abio.ps1 -Name try -Hours 0             # run until Esc
      tools\abio.ps1 -Name night -Hours 12 -Percent 100 -Priority Normal   # all of it, overnight

    How much of the machine it takes:
      -Percent N    use N% of the logical processors (default 75; 100 = all).
      -Threads T    threads per trial (default 1: a single-node trial cannot use
                    more). Trials at once = Percent x processors / T, at least 1.
                    A world trial (Phase 3) is one multi-node process: give it
                    the threads, e.g. -Threads 8 runs two at once on 20 logical
                    processors at 75%.
      -Parallel N   or exactly N at once (overrides -Percent).

    World trials (Phase 3):
      tools\abio.ps1 -Name w1 -Config configs\world.cfg -Arms baseline -Seeds 1,2 -Threads 8 -AfterReplicator -1 -Hours 1
      -Priority P   Idle, BelowNormal (default) or Normal. Below normal keeps the
                    desktop responsive while trials run; Normal for overnight runs.

    Arms (each changes one thing from configs\abio.cfg):
      baseline   nothing
      shuffled   isa.selectors = shuffled   (D6: the numbering comes from the seed)
      plain      isa.table = nova_v1_plain  (D7)
      inv2       isa.table = nova_v1_inv2   (D8)
      gpu        world.gpus = 400, gpu.wear = 0.001  (Phase 2: GPU on vs off)
      npu        world.npus = 200, npu.wear = 0.001  (Phase 2b: NPU on vs off)
      accel      both of the above
    World arms (Phase 4, each one feature at its suggested values; use with
    -Config configs\world.cfg):
      leak       heat.ambient = 0.05
      transit    edge corruption 2e-4 and link failures 1e-4 (downtime 1000)
      disk       a 4096-codon disk and volatility 1e-5
      salvage    salvage.ratio = 4
      signals    16 signal channels
      lifecycle  wear (endurance 1e9), starvation (below 0.2%, meter 2000, ebb 0.00025), land 0.8 drifting at 1e-5
      phase4     all of them
    Accelerator arms (Phase 5): async ops on the real GPU and NPU with the
    high-volume profile (accel.mode = async):
      gpu_hw     the gpu arm, async
      npu_hw     the npu arm, async
      accel_hw   the accel arm, async
    GPU program arms (Phase 8):
      warps      the gpu arm with warps on (gpu.warps) and sparks born on the GPU
      gpu_world  GPU units against CPU, a third of the nodes CPU-barren, warps on
                 (use with -Config configs\landscape.cfg)
    plus any custom arm given with -ArmSet @{ name = 'key=value;key=value' }.

    Pairing: every arm runs the same seeds, and seed s starts from the same
    random soup in every arm.

    Each trial is runs\abio\<Name>\<arm>\s<seed>\ with stats.csv, nodes.csv,
    replay.log, genebank.txt, lineages.csv, result.txt and stdout.txt. A trial with a
    result.txt is done; the harness is resumable (re-run the same command) and
    restarts any trial that has no result.

    Esc in this window stops every running trial gracefully (through stop files):
    each finishes its outputs and records its end tick, so its replay stays
    exact. Trials not yet started are left for a resume.

    Outputs in runs\abio\<Name>\:
      trials.csv    one row per finished trial
      summary.csv   per arm: trials, detections, exposure (instructions run
                    until detection, or until the end for a miss) and
                    lambda = detections / exposure, the maximum-likelihood rate
                    for exponential waiting times with censoring (D9).
#>
param(
    [string]$Name = 'exp1',
    [string[]]$Arms = @('baseline', 'shuffled', 'plain', 'inv2'),
    [int[]]$Seeds = @(1, 2, 3),
    [hashtable]$ArmSeeds = @{},
    [hashtable]$ArmSet = @{},
    [string[]]$Set = @(),
    [double]$Hours = 12,
    [long]$Ticks = 0,
    [ValidateRange(1, 100)][int]$Percent = 75,
    [ValidateRange(0, 1024)][int]$Threads = 1,
    [int]$Parallel = 0,
    [ValidateSet('Idle', 'BelowNormal', 'Normal')][string]$Priority = 'BelowNormal',
    [long]$AfterReplicator = 50000,
    [string]$Config = 'configs\abio.cfg',
    [string]$Exe = '',
    [switch]$Summarize,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$cpus = [Environment]::ProcessorCount
if ($Threads -le 0) { $Threads = [math]::Max(1, [math]::Floor($cpus * $Percent / 100)) } # 0: one trial's worth
if ($Parallel -le 0) { $Parallel = [math]::Max(1, [math]::Floor($cpus * $Percent / 100 / $Threads)) }
$root = Join-Path $repo "runs\abio\$Name"
if (-not $Exe) { $Exe = Join-Path $repo 'build-release\bin\nova.exe' }
$cfgPath = if ([IO.Path]::IsPathRooted($Config)) { $Config } else { Join-Path $repo $Config }

$builtin = @{
    baseline = ''
    shuffled = 'isa.selectors=shuffled'
    plain    = 'isa.table=nova_v1_plain'
    inv2     = 'isa.table=nova_v1_inv2'
    gpu      = 'world.gpus=400;gpu.wear=0.001'
    npu      = 'world.npus=200;npu.wear=0.001'
    accel    = 'world.gpus=400;gpu.wear=0.001;world.npus=200;npu.wear=0.001'
}
# Phase 4 world arms (design.md 6.10), at the plan's suggested values.
$p4 = [ordered]@{
    leak      = 'heat.ambient=0.05'
    transit   = 'edge.corruption=0.0002;edge.failure=0.0001;edge.downtime=1000'
    disk      = 'world.disk=4096;mem.volatility=0.00001'
    salvage   = 'salvage.ratio=4'
    signals   = 'signal.channels=16'
    lifecycle = 'node.endurance=1000000000;world.land=0.8;world.drift=0.00001;node.fallow_min=2000;node.recover_every=100'
}
foreach ($k in $p4.Keys) { $builtin[$k] = $p4[$k] }
$builtin['phase4'] = ($p4.Values -join ';')
# Phase 5 arms (design.md 6.11): the gpu, npu and accel arms with async ops on
# the real devices and the high-volume profile (accel.mode = async).
$builtin['gpu_hw'] = $builtin['gpu'] + ';accel.mode=async'
$builtin['npu_hw'] = $builtin['npu'] + ';accel.mode=async'
$builtin['accel_hw'] = $builtin['accel'] + ';accel.mode=async'
# Phase 8 arms (design.md 6.14): programs on the GPU. warps: the gpu arm with
# warps on and sparks born on the GPU too. gpu_world: a landscape's GPU units
# placed against its CPU, a third of its nodes CPU-barren, warps on (use with
# -Config configs\landscape.cfg; configs\gpu_world.cfg is the same world).
$builtin['warps'] = $builtin['gpu'] + ';gpu.warps=true;spark.gpu_grant=8'
$builtin['gpu_world'] = 'world.gpus=400;gpu.warps=true;spark.gpu_grant=8;res.cpu.spread=0.8;res.cpu.smooth=0.8;' +
    'res.cpu.barren=0.3;res.gpu.spread=0.8;res.gpu.smooth=0.8;res.gpu.against=cpu;res.gpu.barren=0.2'

function Read-Result($path) {
    $r = @{}
    foreach ($line in [IO.File]::ReadAllLines($path)) {
        $i = $line.IndexOf('=')
        if ($i -gt 0) { $r[$line.Substring(0, $i).Trim()] = $line.Substring($i + 1).Trim() }
    }
    return $r
}

function Write-Summary {
    if (-not (Test-Path $root)) { Write-Host "abio: nothing in $root"; return }
    $rows = @()
    foreach ($f in Get-ChildItem $root -Recurse -Filter result.txt) {
        $r = Read-Result $f.FullName
        $arm = $f.Directory.Parent.Name
        $detected = $r['detected'] -eq 'true'
        $instr = [double]$r['instructions']
        $exposure = if ($detected) { [double]$r['detect_instructions'] } else { $instr }
        $rows += [pscustomobject]@{
            arm = $arm; seed = [long]$r['seed']; detected = $detected
            detect_tick = $r['detect_tick']; detect_instructions = $r['detect_instructions']
            exposure = $exposure; instructions = $instr; end_tick = [long]$r['end_tick']
            stop = $r['stop']; seconds = [double]$r['seconds']; links = [long]$r['links']
            lineages_founded = [long]$r['lineages_founded']; max_depth = [long]$r['max_depth']
            detect_len = $r['detect_len']; detect_genome = $r['detect_genome']
            colonist = $r['colonist'] -eq 'true'; colonist_tick = $r['colonist_tick']
            colonisations = [long]$r['colonisations']
            gpu_links = [long]$r['gpu_links']; gpu_replicator = $r['gpu_replicator'] -eq 'true'
            npu_user = $r['npu_user'] -eq 'true'
            accel_gpu = $r['accel_gpu']; accel_npu = $r['accel_npu']
            accel_device_ops = [long]$r['accel_device_ops']; accel_npu_ops = [long]$r['accel_npu_ops']
            gpu_native = $r['gpu_native'] -eq 'true'; gpu_native_tick = $r['gpu_native_tick']
            warp_device = $r['warp_device']
        }
    }
    $rows = $rows | Sort-Object arm, seed
    $rows | Export-Csv -NoTypeInformation -Path (Join-Path $root 'trials.csv')
    $summary = foreach ($g in ($rows | Group-Object arm)) {
        $det = @($g.Group | Where-Object { $_.detected }).Count
        $exp = ($g.Group | Measure-Object exposure -Sum).Sum
        $lambda = if ($exp -gt 0) { $det / $exp } else { 0 }
        [pscustomobject]@{
            arm = $g.Name; trials = $g.Count; detections = $det
            exposure_instructions = '{0:E3}' -f $exp
            lambda_per_instruction = '{0:E3}' -f $lambda
            expected_wait_instructions = if ($det -gt 0) { '{0:E3}' -f ($exp / $det) } else { '' }
            hours = '{0:N2}' -f (($g.Group | Measure-Object seconds -Sum).Sum / 3600)
        }
    }
    $summary | Export-Csv -NoTypeInformation -Path (Join-Path $root 'summary.csv')
    $summary | Format-Table -AutoSize | Out-String | Write-Host
}

if ($Summarize) { Write-Summary; exit 0 }

# --- build the Release engine (the same way tools\bench.ps1 does) -----------
if (-not $NoBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio with the C++ build tools found.' }
    if (-not $env:VSCMD_VER) {
        & cmd /c "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >NUL 2>&1 && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
        }
    }
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    $build = Join-Path $repo 'build-release'
    if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
        & $cmake -S $repo -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release -DNOVA_BUILD_TESTS=OFF | Out-Null
    }
    & $cmake --build $build --target nova | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'abio: Release build failed' }
}
if (-not (Test-Path $Exe)) { throw "abio: $Exe not found" }

# --- the job list ---------------------------------------------------------------
$jobs = New-Object System.Collections.ArrayList
foreach ($arm in $Arms) {
    $sets = if ($ArmSet.ContainsKey($arm)) { $ArmSet[$arm] } elseif ($builtin.ContainsKey($arm)) { $builtin[$arm] } else { throw "abio: unknown arm '$arm' (give it with -ArmSet)" }
    $trialSeeds = if ($ArmSeeds.ContainsKey($arm)) { @($ArmSeeds[$arm]) } else { $Seeds }
    foreach ($s in $trialSeeds) {
        $dir = Join-Path $root "$arm\s$s"
        if (Test-Path (Join-Path $dir 'result.txt')) { continue } # done (resumable)
        [void]$jobs.Add([pscustomobject]@{ arm = $arm; seed = $s; dir = $dir; sets = $sets })
    }
}
$seconds = [long]($Hours * 3600)
Write-Host ("abio: {0}: {1} trials to run, {2} at a time on {3} thread(s) each ({4:N0}% of {5} logical processors), {6} each, {7} priority. Esc stops." -f `
        $Name, $jobs.Count, $Parallel, $Threads, (100.0 * $Parallel * $Threads / $cpus), $cpus,
    $(if ($seconds -gt 0) { "$Hours h" } else { 'until Esc' }), $Priority)

function Quote($s) { if ($s -match '[\s"]') { '"' + ($s -replace '"', '\"') + '"' } else { $s } }

function Start-Trial($job) {
    if (Test-Path $job.dir) { Remove-Item -Recurse -Force $job.dir } # an unfinished earlier attempt
    New-Item -ItemType Directory -Force $job.dir | Out-Null
    $kv = @("seed=$($job.seed)", "run.seconds=$seconds", "run.after_replicator=$AfterReplicator",
        "output.csv=$(Join-Path $job.dir 'stats.csv')", "output.nodes=$(Join-Path $job.dir 'nodes.csv')",
        "output.replay=$(Join-Path $job.dir 'replay.log')",
        "output.genebank=$(Join-Path $job.dir 'genebank.txt')", "output.lineages=$(Join-Path $job.dir 'lineages.csv')",
        "output.result=$(Join-Path $job.dir 'result.txt')", 'output.snapshot=')
    if ($Ticks -gt 0) { $kv += "run.ticks=$Ticks" }
    # nova sets its own priority at start (Phase 7b): tell it ours, so the two never race.
    $kv += "budget.priority=$(@{ Idle = 'idle'; BelowNormal = 'below_normal'; Normal = 'normal' }[$Priority])"
    if ($job.sets) { $kv += ($job.sets -split ';' | Where-Object { $_.Trim() }) }
    $kv += $Set
    $argv = @('run', (Quote $cfgPath), '--threads', "$Threads", '--no-esc', '--stop-file', (Quote (Join-Path $job.dir 'stop')))
    foreach ($x in $kv) { $argv += '--set'; $argv += (Quote $x) }
    $p = Start-Process -FilePath $Exe -ArgumentList ($argv -join ' ') -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $job.dir 'stdout.txt') -RedirectStandardError (Join-Path $job.dir 'stderr.txt')
    try { $p.PriorityClass = $Priority } catch {}
    return [pscustomobject]@{ job = $job; proc = $p }
}

# --- the queue ------------------------------------------------------------------
$running = New-Object System.Collections.ArrayList
$next = 0
$stopping = $false
$finished = 0
$detections = 0
$t0 = Get-Date
$lastStatus = Get-Date
while ($next -lt $jobs.Count -or $running.Count -gt 0) {
    # Esc: stop everything gracefully (only in an interactive console).
    try {
        while (-not $stopping -and [Console]::KeyAvailable) {
            if ([Console]::ReadKey($true).Key -eq 'Escape') {
                $stopping = $true
                Write-Host 'abio: Esc -- stopping every running trial (outputs are finished, replays stay exact)...'
                foreach ($r in $running) { New-Item -ItemType File -Force (Join-Path $r.job.dir 'stop') | Out-Null }
            }
        }
    } catch {}
    foreach ($r in @($running)) {
        if ($r.proc.HasExited) {
            [void]$running.Remove($r)
            $res = Join-Path $r.job.dir 'result.txt'
            if (Test-Path $res) {
                $finished++
                $rr = Read-Result $res
                $tag = if ($rr['detected'] -eq 'true') { $detections++; "REPLICATOR at tick $($rr['detect_tick'])" } else { 'no replicator' }
                Write-Host ("abio: {0} s{1} done ({2}, stop: {3}, {4} ticks)" -f $r.job.arm, $r.job.seed, $tag, $rr['stop'], $rr['end_tick'])
            } else {
                Write-Host ("abio: {0} s{1} exited with code {2} and no result; see {3}" -f $r.job.arm, $r.job.seed, $r.proc.ExitCode, $r.job.dir)
            }
        }
    }
    while (-not $stopping -and $next -lt $jobs.Count -and $running.Count -lt $Parallel) {
        [void]$running.Add((Start-Trial $jobs[$next]))
        $next++
    }
    if ($stopping -and $running.Count -eq 0) { break }
    if (((Get-Date) - $lastStatus).TotalMinutes -ge 10) {
        $lastStatus = Get-Date
        Write-Host ("abio: {0:N1} h: {1} running, {2} finished, {3} waiting, {4} replicators so far" -f `
                ((Get-Date) - $t0).TotalHours, $running.Count, $finished, ($jobs.Count - $next), $detections)
    }
    Start-Sleep -Milliseconds 500
}
if ($stopping -and $next -lt $jobs.Count) { Write-Host "abio: $($jobs.Count - $next) trials not started; re-run the same command to resume." }
Write-Summary
