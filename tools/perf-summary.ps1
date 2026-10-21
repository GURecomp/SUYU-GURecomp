<#
.SYNOPSIS
  Summarises user\nvn_perf.txt (written with [Debug] gpu_diagnostics = true) into one table per scene and state.

.DESCRIPTION
  A run is one game launch. During the run press Numpad . (decimal) once at the start of each protocol scene: that writes an
  "M n=<k>" line. The FSR state (off / 3.1 / 4, objvel, blockmatch) is read from every one-second S line, so toggling FSR with
  Numpad1 / Ctrl+Numpad1 / Numpad9 inside a scene splits that scene into rows automatically. The first -Skip seconds after
  each scene mark or state change are dropped (settling, history reset, shader builds).

  The main-build baseline has no nvn_perf.txt: give its fps per scene in a CSV with -Baseline (lines "scene,fps", scene =
  number k or name from -Scenes; '#' comments allowed). Rows are tabulated and the deltas computed from them.

.EXAMPLE
  .\perf-summary.ps1 -Perf "E:\...\user\nvn_perf.txt" -Scenes village_stand,village_move,quest,camp,save -Baseline main.csv
  .\perf-summary.ps1 -Perf run1.txt -Csv out.csv
#>
param(
    [string]$Perf = "E:\Gaming\GU\GPU_TEST\MONSTER HUNTER GENERATIONS ULTIMATE\user\nvn_perf.txt",
    [string[]]$Scenes = @("village_stand", "village_move", "quest", "camp", "save"),
    [string]$Baseline = "",
    [int]$Skip = 3,
    [string]$Csv = ""
)

function Parse-Kv([string]$line) {
    $h = @{}
    foreach ($t in $line.Split(" ", [StringSplitOptions]::RemoveEmptyEntries)) {
        $i = $t.IndexOf("=")
        if ($i -gt 0) { $h[$t.Substring(0, $i)] = $t.Substring($i + 1) }
    }
    return $h
}
function Num($h, $k) { if ($h.ContainsKey($k)) { [double]::Parse($h[$k], [Globalization.CultureInfo]::InvariantCulture) } else { 0.0 } }
function SceneName([int]$n) { if ($n -ge 1 -and $n -le $Scenes.Count) { $Scenes[$n - 1] } elseif ($n -eq 0) { "(before first mark)" } else { "scene$n" } }

if (-not (Test-Path $Perf)) { Write-Error "no such file: $Perf (run the game with gpu_diagnostics = true)"; exit 1 }
$lines = Get-Content $Perf

# ---------------------------------------------------------------------------------------------------------------------
# Automatic benchmark runs ([Debug] nvn_bench = 1): S lines carry " bench=<run>:<idx>:<p|w|m>:<name> bscene=.. bad=.. drift=..".
# Only the 'm' (measure) seconds are used. Old logs without bench tags fall through to the manual-marker report below.
# ---------------------------------------------------------------------------------------------------------------------
if ($lines | Where-Object { $_.StartsWith("S ") -and $_.Contains(" bench=") } | Select-Object -First 1) {
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $mainFps = @{}
    if ($Baseline -and (Test-Path $Baseline)) {
        foreach ($b in Get-Content $Baseline) {
            if ($b.Trim().StartsWith("#") -or -not $b.Contains(",")) { continue }
            $p = $b.Split(","); $mainFps[$p[0].Trim()] = [double]::Parse($p[1].Trim(), $inv)
        }
    }
    $runs = [ordered]@{}   # run -> @{ scene; wins = ordered idx -> @{ name; fsr; secs = list of S hashtables; bad; drift; warm } }
    foreach ($l in $lines) {
        if (-not $l.StartsWith("S ") -or -not $l.Contains(" bench=")) { continue }
        $h = Parse-Kv $l
        $bp = $h["bench"].Split(":")
        $run = $bp[0]; $idx = [string]$bp[1]; $ph = $bp[2]; $name = $bp[3]
        if (-not $runs.Contains($run)) { $runs[$run] = @{ scene = $h["bscene"]; wins = [ordered]@{} } }
        $w = $runs[$run].wins
        if (-not $w.Contains($idx)) { $w[$idx] = @{ name = $name; secs = (New-Object System.Collections.ArrayList); bad = 0; drift = 0; mism = 0; nw = 0 } }
        $e = $w[$idx]
        if ($ph -eq "w") { $e.nw++ }
        if ($ph -ne "m") { continue }
        $want = if ($name -like "fsr_off*" -or $name -eq "emu") { "off" } elseif ($name -like "fsr4*") { "4" } else { "3.1" }
        if ($h["fsr"] -ne $want) { $e.mism++ }
        if ([int]$h["bad"] -gt 0) { $e.bad += [int]$h["bad"] }
        if ([int]$h["drift"] -gt 0) { $e.drift++ }
        [void]$e.secs.Add($h)
    }
    $allCsv = New-Object System.Collections.ArrayList
    foreach ($run in $runs.Keys) {
        $scene = $runs[$run].scene
        $rows = New-Object System.Collections.ArrayList
        $stage = @{}
        foreach ($idx in $runs[$run].wins.Keys) {
            $e = $runs[$run].wins[$idx]; $g = $e.secs
            if ($g.Count -eq 0) { continue }
            $nf = ($g | ForEach-Object { Num $_ "n" } | Measure-Object -Sum).Sum
            $wavg = { param($k) $s = 0.0; foreach ($h in $g) { $s += (Num $h $k) * (Num $h "n") }; $s / $nf }
            $mean = { param($k) ($g | ForEach-Object { Num $_ $k } | Measure-Object -Average).Average }
            $first = $g[0]
            $notes = @()
            if ($e.bad -gt 0) { $notes += "INVALID: $($e.bad) non-scene frames (menu/loading)" }
            if ($e.drift -gt 0) { $notes += "INVALID: a key changed a toggle" }
            if ($e.mism -gt 0) { $notes += "INVALID: backend differs from the state ($($e.mism) s)" }
            $r = [pscustomobject]@{
                Idx = $idx; State = $e.name; Secs = $g.Count; Fps = [math]::Round((& $mean "fps"), 1)
                P50 = [math]::Round((& $mean "wall_p50"), 2); P95 = [math]::Round((& $mean "wall_p95"), 2); P99 = [math]::Round((& $mean "wall_p99"), 2)
                Gpu = [math]::Round((& $wavg "gpu_avg"), 2); Util = [math]::Round((& $mean "gpu_util"), 0)
                Gap = [math]::Round((& $wavg "gap"), 2); RCpu = [math]::Round((& $wavg "rcpu"), 2); Wait = [math]::Round((& $wavg "wait"), 2)
                Geo = $first["objvel"]; Bm = $first["blockmatch"]; Anim = $first["anim"]
                dFpsVsOff = ""; FsrMs = ""; dGpuVsOff = ""; dFpsVsMain = ""; Valid = ($notes.Count -eq 0); Notes = ($notes -join "; ")
            }
            $st = @{}
            foreach ($h in $g) {
                foreach ($k in $h.Keys) {
                    if ($k.StartsWith("g.")) {
                        $v = [double]::Parse($h[$k].Split("/")[0], $inv)
                        if (-not $st.ContainsKey($k)) { $st[$k] = 0.0 }
                        $st[$k] += $v * (Num $h "n")
                    }
                }
            }
            foreach ($k in @($st.Keys)) { $st[$k] = $st[$k] / $nf }
            $stage["$idx"] = $st
            [void]$rows.Add($r)
        }
        Write-Host "`n================ RUN $run  scene = $scene ================"
        $offs = @($rows | Where-Object { $_.State -like "fsr_off*" -and $_.Valid })
        $offFps = $null; $offGpu = $null
        if ($offs.Count -gt 0) {
            $offFps = ($offs | Measure-Object Fps -Average).Average; $offGpu = ($offs | Measure-Object Gpu -Average).Average
        }
        $offAll = @($rows | Where-Object { $_.State -like "fsr_off*" })
        if ($offAll.Count -ge 2) {
            $a = $offAll[0].Fps; $b = $offAll[$offAll.Count - 1].Fps
            $d = 100.0 * [math]::Abs($a - $b) / (($a + $b) / 2)
            if ($d -gt 5.0) { Write-Host ("WARNING: the two FSR-off bookends differ by {0:N1}% ({1} vs {2} fps): drift or the scene changed; compare with care" -f $d, $a, $b) -ForegroundColor Yellow }
            else { Write-Host ("FSR-off bookends agree ({0:N1}% apart, {1} / {2} fps)" -f $d, $a, $b) }
        } else { Write-Host "WARNING: fewer than two FSR-off windows (bookend check impossible)" -ForegroundColor Yellow }
        foreach ($r in $rows) {
            if ($offFps -and $r.State -notlike "fsr_off*") {
                $r.dFpsVsOff = [math]::Round($r.Fps - $offFps, 1)
                $r.FsrMs = [math]::Round(1000.0 / $r.Fps - 1000.0 / $offFps, 2)
                $r.dGpuVsOff = [math]::Round($r.Gpu - $offGpu, 2)
            }
            if ($mainFps.ContainsKey($scene)) { $r.dFpsVsMain = [math]::Round($r.Fps - $mainFps[$scene], 1) }
            if (-not $r.Valid) { Write-Host ("WARNING: window {0} ({1}) {2}" -f $r.Idx, $r.State, $r.Notes) -ForegroundColor Yellow }
        }
        $rows | Format-Table Idx, State, Geo, Bm, Secs, Fps, P50, P95, P99, Gpu, Gap, RCpu, Wait, dFpsVsOff, FsrMs, dGpuVsOff, dFpsVsMain, Valid -AutoSize
        if ($mainFps.ContainsKey($scene)) { Write-Host ("main build: {0} fps ({1}); dFpsVsMain = this build minus main" -f $mainFps[$scene], $scene) }
        Write-Host "FsrMs = frame time (1000/fps) minus the average FSR-off bookend frame time. Geo/Bm = geometry object vectors / block matching ON(1)/OFF(0) (block matching is ON by default)."
        $labels = @{}
        foreach ($k in $stage.Keys) { foreach ($l in $stage[$k].Keys) { if ($stage[$k][$l] -ge 0.05) { $labels[$l] = $true } } }
        $out = foreach ($l in ($labels.Keys | Sort-Object)) {
            $o = [ordered]@{ Stage = $l.Substring(2) }
            foreach ($r in $rows) { $o["$($r.Idx):$($r.State)"] = if ($stage["$($r.Idx)"].ContainsKey($l)) { [math]::Round($stage["$($r.Idx)"][$l], 2) } else { 0 } }
            [pscustomobject]$o
        }
        Write-Host "GPU ms per frame by stage (stages >= 0.05 ms):"
        $out | Format-Table -AutoSize
        foreach ($r in $rows) { $r | Add-Member -NotePropertyName Run -NotePropertyValue $run -Force; $r | Add-Member -NotePropertyName Scene -NotePropertyValue $scene -Force; [void]$allCsv.Add($r) }
    }
    if ($Csv) { $allCsv | Export-Csv -NoTypeInformation -Path $Csv; Write-Host "wrote $Csv" }
    exit 0
}

$scene = 0; $prevKey = ""; $age = 0
$groups = [ordered]@{}   # "scene|state" -> list of S hashtables
$spikes = @{}            # scene -> count
foreach ($l in $lines) {
    if ($l.StartsWith("M ")) { $scene = [int](Parse-Kv $l)["n"]; $prevKey = ""; continue }
    if ($l.StartsWith("X ")) { if (-not $spikes.ContainsKey($scene)) { $spikes[$scene] = 0 }; $spikes[$scene]++; continue }
    if (-not $l.StartsWith("S ")) { continue }
    $h = Parse-Kv $l
    $state = "fsr=" + $h["fsr"]
    if ($h["fsr"] -and $h["fsr"] -ne "off") { $state += " objvel=" + $h["objvel"] + " bm=" + $h["blockmatch"] }
    $key = "$scene|$state"
    if ($key -ne $prevKey) { $age = 0; $prevKey = $key }
    $age++
    if ($age -le $Skip) { continue }
    if (-not $groups.Contains($key)) { $groups[$key] = New-Object System.Collections.ArrayList }
    [void]$groups[$key].Add($h)
}

# baseline csv
$base = @{}
if ($Baseline -and (Test-Path $Baseline)) {
    foreach ($b in Get-Content $Baseline) {
        if ($b.Trim().StartsWith("#") -or -not $b.Contains(",")) { continue }
        $p = $b.Split(","); $name = $p[0].Trim(); $fps = [double]::Parse($p[1].Trim(), [Globalization.CultureInfo]::InvariantCulture)
        if ($name -match '^\d+$') { $name = SceneName ([int]$name) }
        $base[$name] = $fps
    }
}

$rows = New-Object System.Collections.ArrayList
$stageSum = @{}
foreach ($key in $groups.Keys) {
    $sc, $state = $key.Split("|", 2)
    $g = $groups[$key]
    $nf = ($g | ForEach-Object { Num $_ "n" } | Measure-Object -Sum).Sum
    if ($nf -le 0) { continue }
    function WAvg([string]$k) { $s = 0.0; foreach ($h in $g) { $s += (Num $h $k) * (Num $h "n") }; $s / $nf }
    function Mean([string]$k) { ($g | ForEach-Object { Num $_ $k } | Measure-Object -Average).Average }
    function Max([string]$k) { ($g | ForEach-Object { Num $_ $k } | Measure-Object -Maximum).Maximum }
    $r = [pscustomobject]@{
        Scene = SceneName ([int]$sc); State = $state; Secs = $g.Count
        Fps = [math]::Round((Mean "fps"), 1)
        P50 = [math]::Round((Mean "wall_p50"), 2); P95 = [math]::Round((Mean "wall_p95"), 2); P99 = [math]::Round((Mean "wall_p99"), 2)
        Max = [math]::Round((Max "wall_max"), 1)
        Gap = [math]::Round((WAvg "gap"), 2); RCpu = [math]::Round((WAvg "rcpu"), 2); Wait = [math]::Round((WAvg "wait"), 2); Stall = [math]::Round((WAvg "stall"), 2)
        Gpu = [math]::Round((WAvg "gpu_avg"), 2); GpuP99 = [math]::Round((Mean "gpu_p99"), 2); Util = [math]::Round((Mean "gpu_util"), 0)
        Spikes = $(if ($spikes.ContainsKey([int]$sc)) { $spikes[[int]$sc] } else { 0 })
        dFpsVsOff = ""; dFpsVsMain = ""; dGpuVsOff = ""
    }
    # per-stage GPU ms per frame
    $st = @{}
    foreach ($h in $g) {
        foreach ($k in $h.Keys) {
            if ($k.StartsWith("g.")) {
                $v = [double]::Parse($h[$k].Split("/")[0], [Globalization.CultureInfo]::InvariantCulture)
                if (-not $st.ContainsKey($k)) { $st[$k] = 0.0 }
                $st[$k] += $v * (Num $h "n")
            }
        }
    }
    foreach ($k in @($st.Keys)) { $st[$k] = $st[$k] / $nf }
    $stageSum[$key] = $st
    [void]$rows.Add($r)
}

# deltas
foreach ($r in $rows) {
    $off = $rows | Where-Object { $_.Scene -eq $r.Scene -and $_.State -eq "fsr=off" } | Select-Object -First 1
    if ($off -and $r.State -ne "fsr=off") {
        $r.dFpsVsOff = [math]::Round($r.Fps - $off.Fps, 1)
        $r.dGpuVsOff = [math]::Round($r.Gpu - $off.Gpu, 2)
    }
    if ($base.ContainsKey($r.Scene)) { $r.dFpsVsMain = [math]::Round($r.Fps - $base[$r.Scene], 1) }
}

Write-Host "`n=== Frame time (ms) and CPU/GPU split, per scene and state (fps/ms means over seconds; gap includes the game's pacing sleep) ==="
$rows | Format-Table Scene, State, Secs, Fps, P50, P95, P99, Max, Gap, RCpu, Wait, Stall, Gpu, GpuP99, Util, Spikes, dFpsVsOff, dFpsVsMain, dGpuVsOff -AutoSize
if ($base.Count) {
    Write-Host "=== Main build baseline (manual fps) ==="
    $base.GetEnumerator() | Sort-Object Name | ForEach-Object { "{0,-16} {1,8:N1} fps" -f $_.Key, $_.Value }
}

Write-Host "`n=== GPU ms per frame by stage (only stages >= 0.05 ms in some state; fsr_* / fsr4_* / camera_mv / pass_* are intervals ending at that mark) ==="
$labels = @{}
foreach ($k in $stageSum.Keys) { foreach ($l in $stageSum[$k].Keys) { if ($stageSum[$k][$l] -ge 0.05) { $labels[$l] = $true } } }
foreach ($sc in ($rows | Select-Object -ExpandProperty Scene -Unique)) {
    Write-Host "--- $sc"
    $states = $rows | Where-Object { $_.Scene -eq $sc } | Select-Object -ExpandProperty State
    $out = foreach ($l in ($labels.Keys | Sort-Object)) {
        $o = [ordered]@{ Stage = $l.Substring(2) }
        $any = $false
        foreach ($s in $states) {
            $key = ($groups.Keys | Where-Object { $_ -like "*|$s" -and (SceneName ([int]($_.Split("|")[0]))) -eq $sc } | Select-Object -First 1)
            $v = if ($key -and $stageSum[$key].ContainsKey($l)) { [math]::Round($stageSum[$key][$l], 2) } else { 0 }
            if ($v -ge 0.05) { $any = $true }
            $o[$s] = $v
        }
        if ($any) { [pscustomobject]$o }
    }
    $out | Format-Table -AutoSize
}
Write-Host "FSR attribution: FSR cost ~ (state GPU total) - (fsr=off GPU total) in the Gpu column; capture (transform feedback) cost sits inside the pass_* rows"
Write-Host "of the game's own draws: compare objvel=0 vs objvel=1 rows (Numpad9) to separate it."

if ($Csv) { $rows | Export-Csv -NoTypeInformation -Path $Csv; Write-Host "wrote $Csv" }
