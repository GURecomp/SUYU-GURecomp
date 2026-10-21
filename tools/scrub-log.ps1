<#
.SYNOPSIS
  Removes personal data (paths, Windows user name, computer name) from an existing suyu log or renderer report.

.DESCRIPTION
  Same rules as the built-in redaction ([Debug] log_redact_paths, src/common/log_redact.cpp), for files written by
  older builds or with redaction off. Keep the two in step.

    <GameDir>\...        -> <game>\...        (install folder; default: derived from the file's location when it
                                              sits in <game>\user\... , else give -GameDir)
    <UserDir>\...        -> <user>\...        (data folder, <game>\user by default)
    X:\Users\<name>\...  -> %USERPROFILE%\... (any user profile), /home/<n>, /Users/<n> -> ~
    any other absolute path -> <path>/<file name>
    the Windows user name / computer name as bare words -> <username> / <computername>
  Relative parts after a placeholder stay. The input file is never modified; the result goes to -OutPath
  (default: <name>.scrubbed<ext> beside the input).

  IPv4 addresses -> <ip-N lan|vpn|public> (N = order of first appearance in the file, same IP = same tag, :port stays,
  127.x and 0.0.0.0 unchanged).
  Not covered: free-text names that are neither the user nor the computer name (e.g. nicknames), IPv6, and Unix absolute paths outside /home and /Users.
  Look at the output before you share it.

.EXAMPLE
  .\scrub-log.ps1 -Path "E:\Game\user\log\1234_suyu_log.txt"
  .\scrub-log.ps1 -Path some_log.txt -GameDir "D:\Games\MHGU" -ExtraNames MyName,MY-PC -OutPath clean.txt
#>
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$OutPath = "",
    [string]$GameDir = "",
    [string]$UserDir = "",
    [string[]]$ExtraNames = @()
)

$ErrorActionPreference = "Stop"
$src = (Resolve-Path -LiteralPath $Path).Path
if (-not $OutPath) {
    $OutPath = Join-Path (Split-Path $src -Parent) ([IO.Path]::GetFileNameWithoutExtension($src) + ".scrubbed" + [IO.Path]::GetExtension($src))
}
if ((Resolve-Path -LiteralPath (Split-Path $OutPath -Parent) -ErrorAction SilentlyContinue).Path -and
    ([IO.Path]::GetFullPath($OutPath) -eq $src)) { throw "-OutPath must differ from -Path" }

# Roots: <user> = ...\user, <game> = its parent, found by walking up from the file when not given.
if (-not $UserDir -or -not $GameDir) {
    $d = Split-Path $src -Parent
    while ($d) {
        if ((Split-Path $d -Leaf) -ieq "user") {
            if (-not $UserDir) { $UserDir = $d }
            if (-not $GameDir) { $GameDir = Split-Path $d -Parent }
            break
        }
        $d = Split-Path $d -Parent
    }
}
$roots = New-Object System.Collections.Generic.List[object]
function Add-Root([string]$p, [string]$ph) {
    if (-not $p) { return }
    $p = $p.TrimEnd('\', '/')
    if ($p.Length -lt 2) { return }
    # Escape() turns '\' into '\\' ; make every separator class-insensitive.
    $pat = [regex]::Escape($p) -replace '\\\\|/', '[\\/]'
    $roots.Add([pscustomobject]@{ Len = $p.Length; Rx = [regex]::new("(?<![A-Za-z0-9_.])" + $pat + "(?![A-Za-z0-9])", 'IgnoreCase,Compiled'); Ph = $ph })
}
Add-Root $UserDir "<user>"
Add-Root $GameDir "<game>"
if ($env:USERPROFILE) { Add-Root $env:USERPROFILE "%USERPROFILE%" }
if ($env:HOME -and $env:HOME -ne $env:USERPROFILE) { Add-Root $env:HOME "~" }
$rootList = @($roots | Sort-Object Len -Descending)

$rxProfile = [regex]::new('(?<![A-Za-z0-9_.])[A-Za-z]:[\\/]Users[\\/][^\\/:*?"<>|\x00-\x1f\s]+', 'IgnoreCase,Compiled')
$rxHome = [regex]::new('(?<![A-Za-z0-9_.])/(?:home|Users)/[^/\s:*?"<>|]+', 'Compiled')
$rxAbs = [regex]::new('(?<![A-Za-z0-9_.])(?:[A-Za-z]:[\\/]|\\\\(?=[A-Za-z0-9]))(?:[^\\/:*?"<>|\x00-\x1f]+[\\/])*([^\\/:*?"<>|\x00-\x1f\s]*)', 'Compiled')
$trailing = [char[]]@('.', ',', ';', ')', ']', "'", '`')
$evalAbs = [System.Text.RegularExpressions.MatchEvaluator] {
    param($m)
    $file = $m.Groups[1].Value
    $trim = $file.TrimEnd($trailing)
    "<path>/" + $trim + $file.Substring($trim.Length)
}

$stop = @("user", "admin", "administrator", "default", "public", "guest", "test", "game", "owner", "desktop", "home",
    "root", "path", "username", "computername", "userprofile", "suyu", "yuzu", "player")
$names = @($env:USERNAME, $env:COMPUTERNAME) + $ExtraNames | Where-Object { $_ -and $_.Length -ge 4 -and ($stop -notcontains $_.ToLower()) } | Select-Object -Unique | Sort-Object Length -Descending
$nameRx = @()
foreach ($n in $names) {
    $ph = if ($n -ieq $env:COMPUTERNAME) { "<computername>" } else { "<username>" }
    $nameRx += [pscustomobject]@{ Rx = [regex]::new("(?<![A-Za-z0-9])" + [regex]::Escape($n) + "(?![A-Za-z0-9])", 'IgnoreCase,Compiled'); Ph = $ph }
}

# IPv4 -> <ip-N lan|vpn|public>, N = order of first appearance in this file; :port stays; 127.x / 0.0.0.0 unchanged.
$ipIds = @{}
$rxIp = [regex]::new('(?<![A-Za-z0-9_.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?![A-Za-z0-9_])(?!\.\d)', 'Compiled')
$evalIp = [System.Text.RegularExpressions.MatchEvaluator] {
    param($m)
    $o = 1..4 | ForEach-Object { [int]$m.Groups[$_].Value }
    if (($o | Where-Object { $_ -gt 255 })) { return $m.Value }
    if ($o[0] -eq 127 -or ($o[0] -eq 0 -and $o[1] -eq 0 -and $o[2] -eq 0 -and $o[3] -eq 0)) { return $m.Value }
    $key = $o -join '.'
    if (-not $ipIds.ContainsKey($key)) { $ipIds[$key] = $ipIds.Count + 1 }
    $cls = "public"
    if ($o[0] -eq 10 -or ($o[0] -eq 172 -and $o[1] -ge 16 -and $o[1] -le 31) -or ($o[0] -eq 192 -and $o[1] -eq 168) -or ($o[0] -eq 169 -and $o[1] -eq 254)) { $cls = "lan" }
    elseif ($o[0] -eq 26) { $cls = "vpn" }
    "<ip-$($ipIds[$key]) $cls>"
}

function Scrub-Line([string]$line) {
    foreach ($r in $rootList) { $line = $r.Rx.Replace($line, $r.Ph) }
    $line = $rxProfile.Replace($line, '%USERPROFILE%')
    $line = $rxHome.Replace($line, '~')
    $line = $rxAbs.Replace($line, $evalAbs)
    foreach ($n in $nameRx) { $line = $n.Rx.Replace($line, $n.Ph) }
    $rxIp.Replace($line, $evalIp)
}

$sw = New-Object System.IO.StreamWriter($OutPath, $false, (New-Object System.Text.UTF8Encoding($false)))
$count = 0
try {
    foreach ($line in [System.IO.File]::ReadLines($src)) {
        $sw.WriteLine((Scrub-Line $line))
        $count++
    }
}
finally { $sw.Dispose() }
Write-Host "scrubbed $count lines -> $OutPath"
if ($rootList.Count) { Write-Host ("roots: " + (($rootList | ForEach-Object { $_.Ph }) -join ", ")) }
