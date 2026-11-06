#Requires -Version 5.1
<#
    Phase 1b pilot experiments (THROWAWAY -- see README.md).

    Re-runs the experiment sets behind the numbers in docs/phase1b-plan.md
    section 1. Each run is one diag.exe process on one node, out to 50k ticks;
    a set's runs go in parallel. Outputs: build/out/<set>/<run>.txt (git-ignored).

        .\pilots\phase1b\build.ps1
        .\pilots\phase1b\run.ps1 -Set upkeep
        .\pilots\phase1b\run.ps1 -Set all -Parallel 15

    Sets: clog, alloc, share, upkeep, surface, predation, cpuwear, all.
#>
param(
    [ValidateSet('clog', 'alloc', 'share', 'upkeep', 'surface', 'predation', 'cpuwear', 'all')]
    [string]$Set = 'all',
    [int]$Parallel = 8,
    [int]$MaxTick = 50000   # last checkpoint to run to (100, 1000, 5000, 10000, 20000 or 50000)
)
$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build'
$exe = Join-Path $build 'diag.exe'
if (-not (Test-Path $exe)) { throw "build first: $(Join-Path $PSScriptRoot 'build.ps1')" }

# Seeded genomes (design.md 6.2 notation). All start with a 10-millicore grant,
# the same as a spark.
$at = ';seed.at = 30000;seed.millicores = 10'
$G = @{
    sketch = "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 INC DIVIDE$at"            # 6.2 sketch: share 1/64
    share  = "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE$at"          # share = own length / 64
    surf1  = "seed.genome = NOP0 SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE$at"     # + 1-NOP surface
    surf3  = "seed.genome = NOP0 NOP1 NOP0 SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE$at"
    pred   = "seed.genome = NOP0 ENGULF SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE$at" # blind ENGULF each cycle, surfaced
    predB  = "seed.genome = ENGULF SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE$at"   # same, no surface (eats its daughters)
    claim  = "seed.genome = ZERO INC ROTB SENSE ALLOC ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE$at" # claims CPU each cycle
}
# Pilot rule toggles (environment variables read by proto.patch).
$UP  = @{ PROTO_UPKEEP = '2'; PROTO_EXP = '1.3333' }   # square-cube memory upkeep, the revision-3 candidate
$LATE = @{ PROTO_SPARK_LATE = '1' }
function Env([hashtable[]]$parts) { $h = @{}; foreach ($p in $parts) { foreach ($k in $p.Keys) { $h[$k] = $p[$k] } }; $h }

# Each run: name, environment, overrides, seed.
$sets = [ordered]@{
    clog = @(
        @('phase1a_65k',  @{}, '', 7),
        @('phase1a_262k', @{}, 'world.soup_size = 262144', 1),
        @('phase1a_1M',   @{}, 'world.soup_size = 1048576', 1)
    )
    alloc = @(
        @('pending_body',      @{ PROTO_PENDING = '1' }, '', 7),
        @('pending_body_cap3', @{ PROTO_PENDING = '1'; PROTO_MULT = '3' }, '', 7),
        @('pending_cpu_cap3',  @{ PROTO_PENDING = '1'; PROTO_PERCPU = '1'; PROTO_MULT = '3' }, '', 7),
        @('share_body_cap3',   @{ PROTO_PENDING = '1'; PROTO_MULT = '3' }, $G.share, 7),
        @('share_cpu_cap3',    @{ PROTO_PENDING = '1'; PROTO_PERCPU = '1'; PROTO_MULT = '3' }, $G.share, 9)
    )
    share = @(
        @('sketch_phase1a', @{}, $G.sketch, 7),
        @('share_phase1a',  @{}, $G.share, 7),
        @('share_phase1a_s8', @{}, $G.share, 8)
    )
    upkeep = @(
        @('linear_0.5', @{ PROTO_UPKEEP = '0.5' }, '', 7),
        @('linear_1',   @{ PROTO_UPKEEP = '1' }, '', 7),
        @('linear_2',   @{ PROTO_UPKEEP = '2' }, '', 7),
        @('cube43_1',   @{ PROTO_UPKEEP = '1'; PROTO_EXP = '1.3333' }, '', 7),
        @('cube43_2',   @{ PROTO_UPKEEP = '2'; PROTO_EXP = '1.3333' }, '', 7),
        @('exp15_1',    @{ PROTO_UPKEEP = '1'; PROTO_EXP = '1.5' }, '', 7)
    )
    surface = @(
        @('bare_s7',  $UP, $G.share, 7), @('bare_s9',  $UP, $G.share, 9),
        @('surf1_s7', $UP, $G.surf1, 7), @('surf1_s9', $UP, $G.surf1, 9),
        @('surf3_s7', $UP, $G.surf3, 7), @('surf3_s9', $UP, $G.surf3, 9)
    )
    predation = @(
        @('random_late', (Env $UP, $LATE), '', 7),
        @('pred_s7',  $UP, $G.pred, 7), @('pred_s9', $UP, $G.pred, 9),
        @('predB_s7', $UP, $G.predB, 7)
    )
    cpuwear = @(
        @('random_f1e-3_late',  (Env $UP, $LATE, @{ PROTO_CPU_FAULT = '0.001' }), '', 7),
        @('random_f1e-3_early', (Env $UP, @{ PROTO_CPU_FAULT = '0.001' }), '', 7),
        @('claim_f0_late',      (Env $UP, $LATE), $G.claim, 7),
        @('claim_f1e-3_late_s7', (Env $UP, $LATE, @{ PROTO_CPU_FAULT = '0.001' }), $G.claim, 7),
        @('claim_f1e-3_late_s9', (Env $UP, $LATE, @{ PROTO_CPU_FAULT = '0.001' }), $G.claim, 9),
        @('claim_f1e-3_early_s7', (Env $UP, @{ PROTO_CPU_FAULT = '0.001' }), $G.claim, 7),
        @('claim_f3e-4_late_s7', (Env $UP, $LATE, @{ PROTO_CPU_FAULT = '0.0003' }), $G.claim, 7),
        @('share_f1e-3_late_s7', (Env $UP, $LATE, @{ PROTO_CPU_FAULT = '0.001' }), $G.share, 7)
    )
}

$names = if ($Set -eq 'all') { @($sets.Keys) } else { @($Set) }
foreach ($s in $names) {
    $out = Join-Path $build "out\$s"
    New-Item -ItemType Directory -Force $out | Out-Null
    Write-Host "=== set '$s' ($($sets[$s].Count) runs, up to $Parallel at once)" -ForegroundColor Cyan
    $running = @()
    foreach ($r in $sets[$s]) {
        while (@($running | Where-Object { -not $_.HasExited }).Count -ge $Parallel) { Start-Sleep -Milliseconds 500 }
        $envs = ($r[1].GetEnumerator() | ForEach-Object { "set $($_.Key)=$($_.Value)&& " }) -join ''
        $file = Join-Path $out "$($r[0]).txt"
        $ov = if ($r[2]) { $r[2] } else { '-' } # '-' = no overrides
        # The outer quotes are for cmd /c, which strips the first and last quote.
        $cmd = "`"$envs`"$exe`" `"$ov`" $($r[3]) $MaxTick > `"$file`"`""
        $running += Start-Process cmd -ArgumentList '/c', $cmd -PassThru -NoNewWindow -WorkingDirectory $build
    }
    $running | Wait-Process
    foreach ($r in $sets[$s]) {
        Write-Host "--- $($r[0])" -ForegroundColor DarkCyan
        Get-Content (Join-Path $out "$($r[0]).txt")
    }
}
