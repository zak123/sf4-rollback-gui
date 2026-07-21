# Synctest soak harness: run Launcher.exe --synctest in a loop, nudge
# each run past the title screen, harvest the game log per run, and
# summarize verdicts. Each run rolls random characters/stage/seed and
# logs a reproduce line, so any divergence can be replayed exactly.
# See docs/netcode-research.md.
#
#   scripts\synctest-soak.ps1 -Runs 20
#   scripts\synctest-soak.ps1 -Runs 5 -Frames 8 -StopOnDivergence
param(
    [int]$Runs = 10,
    [string]$Launcher = "",
    [int]$Frames = 1,
    [int]$TimeoutMinutes = 12,
    [string]$OutDir = "synctest-results",
    [switch]$StopOnDivergence
)

$ErrorActionPreference = "Stop"

if (-not $Launcher) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $candidates = @(
        (Join-Path $scriptDir "Launcher.exe"),
        (Join-Path $scriptDir "..\msvc-build\default\Launcher.exe")
    )
    $Launcher = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Launcher) {
        throw "Launcher.exe not found beside this script or in msvc-build\default- pass -Launcher"
    }
}
$Launcher = (Resolve-Path $Launcher).Path
$launcherDir = Split-Path -Parent $Launcher
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$gameLog = Join-Path $env:APPDATA "sf4e\logs\sf4e.log"
$summary = Join-Path $OutDir "summary.log"
$shell = New-Object -ComObject WScript.Shell

"soak start: $Runs runs, rollback depth $Frames, launcher $Launcher" | Tee-Object -FilePath $summary -Append

function Wait-RunEnd {
    param([datetime]$Deadline)
    $presses = 0
    while ((Get-Date) -lt $Deadline) {
        Start-Sleep -Seconds 10
        $game = Get-Process SSFIV -ErrorAction SilentlyContinue
        if (-not $game) {
            # Divergence assert or crash already ended the game.
            Start-Sleep -Seconds 3
            return
        }
        $started = (Test-Path $gameLog) -and (Select-String -Path $gameLog -Pattern "Synctest session up" -Quiet)
        if (-not $started -and $presses -lt 15) {
            # Nudge past the title screen; only the game window gets keys.
            if ($shell.AppActivate($game.Id)) {
                $shell.SendKeys("{ENTER}")
                $presses++
            }
        }
        $ended = (Test-Path $gameLog) -and (Select-String -Path $gameLog -Pattern "SYNCTEST DIVERGENCE|Synctest ended" -Quiet)
        if ($ended) {
            Start-Sleep -Seconds 5
            return
        }
    }
}

for ($i = 1; $i -le $Runs; $i++) {
    Get-Process SSFIV -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    if (Test-Path $gameLog) {
        Remove-Item $gameLog -Force -ErrorAction SilentlyContinue
    }

    Start-Process -FilePath $Launcher -ArgumentList "--synctest", "--synctest-frames", $Frames -WorkingDirectory $launcherDir
    Wait-RunEnd -Deadline (Get-Date).AddMinutes($TimeoutMinutes)
    Get-Process SSFIV -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    $dest = Join-Path $OutDir ("run-{0:d3}-{1}.log" -f $i, (Get-Date -Format "HHmmss"))
    $verdict = "no game log produced"
    if (Test-Path $gameLog) {
        Copy-Item $gameLog $dest -Force
        $picksMatch = Select-String -Path $dest -Pattern "Synctest picks: (.+?) \(" | Select-Object -First 1
        $picks = if ($picksMatch) { $picksMatch.Matches.Groups[1].Value } else { "" }
        $diverged = Select-String -Path $dest -Pattern "SYNCTEST DIVERGENCE at frame (\d+)" | Select-Object -First 1
        $clean = Select-String -Path $dest -Pattern "Synctest: (\d+) frames verified clean" | Select-Object -Last 1
        if ($diverged) {
            $verdict = "DIVERGENCE at frame $($diverged.Matches.Groups[1].Value)"
        }
        elseif ($clean) {
            $verdict = "clean through $($clean.Matches.Groups[1].Value)+ frames"
        }
        else {
            $verdict = "never reached the battle"
        }
        if ($picks) {
            $verdict = "$verdict [$picks]"
        }
    }
    "run {0:d3}: {1}" -f $i, $verdict | Tee-Object -FilePath $summary -Append
    if ($StopOnDivergence -and $verdict.StartsWith("DIVERGENCE")) {
        break
    }
}

"soak complete- per-run logs in $OutDir" | Tee-Object -FilePath $summary -Append
