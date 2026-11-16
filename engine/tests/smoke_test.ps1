# Smoke tests.
#  1. f3_server --no-data (built-in Lua test map) + f3_testclient (headless bot): protocol round trips.
#  2. If gamedata/server exists (run f3_dataimport first): f3_server on the real data + f3_gametest
#     (real map grid, stats, inventory, walk, pickup, combat, map transition, door). Skipped otherwise.
param(
    [string]$BinDir = "$PSScriptRoot\..\..\build\vs2022\bin",
    [string]$ScriptsDir = "$PSScriptRoot\..\..\scripts",
    [string]$DataDir = "$PSScriptRoot\..\..\gamedata\server",
    [int]$Port = 4047
)
$ErrorActionPreference = "Stop"
$server = Join-Path $BinDir "f3_server.exe"
$bot = Join-Path $BinDir "f3_testclient.exe"
$gametest = Join-Path $BinDir "f3_gametest.exe"
$empty = New-Item -Force "$env:TEMP\f3_empty_stdin.txt"

$saves = Join-Path $env:TEMP ("f3_smoke_saves_" + $PID)   # throw-away account directory, never the real saves/
if (Test-Path $saves) { Remove-Item -Recurse -Force $saves }

function Run-Server([string]$extra, [string]$log, [int]$port) {
    Start-Process -FilePath $server -ArgumentList "--local --port $port --scripts `"$ScriptsDir`" --saves `"$saves`" $extra --run-seconds 90" `
        -RedirectStandardOutput $log -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
}

$exit = 0

# ---- 1. test map
$log1 = Join-Path $env:TEMP "f3_server_smoke.log"
$p = Run-Server "--no-data --allow-guest" $log1 $Port
try {
    Start-Sleep -Milliseconds 800
    & $bot --port $Port --name SmokeBot
    $code = $LASTEXITCODE
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
Write-Host "---- server log (test map) ----"
Get-Content $log1
if ($code -ne 0) { $exit = $code }

# ---- 2. real data
if ((Test-Path (Join-Path $DataDir "manifest.json")) -and (Test-Path $gametest)) {
    $log2 = Join-Path $env:TEMP "f3_server_smoke_data.log"
    $p = Run-Server "--data `"$DataDir`" --dev-kit" $log2 ($Port + 1)
    try {
        Start-Sleep -Milliseconds 2500
        & $gametest --port ($Port + 1)
        $code = $LASTEXITCODE
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
    Write-Host "---- server log (game data) ----"
    Get-Content $log2
    if ($code -ne 0) { $exit = $code }

    # ---- 3. accounts + persistence: register, play, quit; RESTART the server; login and check everything came back
    $state = Join-Path $env:TEMP "f3_acct_state.txt"
    foreach ($phase in 1, 2) {
        $log3 = Join-Path $env:TEMP "f3_server_smoke_acct$phase.log"
        $p = Run-Server "--data `"$DataDir`"" $log3 ($Port + 2)
        try {
            Start-Sleep -Milliseconds 2500
            & $gametest --port ($Port + 2) --accounts $phase --state $state
            $code = $LASTEXITCODE
        } finally {
            if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
        }
        Write-Host "---- server log (accounts, phase $phase) ----"
        Get-Content $log3 | Select-Object -Last 12
        if ($code -ne 0) { $exit = $code; break }
    }

    # ---- 4. doors/containers: state transitions, locked doors block pathing, lockpick, persistence across a server restart
    $dstate = Join-Path $env:TEMP "f3_door_state.txt"
    $dsaves = Join-Path $env:TEMP ("f3_smoke_door_saves_" + $PID)
    if (Test-Path $dsaves) { Remove-Item -Recurse -Force $dsaves }
    foreach ($phase in 1, 2) {
        $log4 = Join-Path $env:TEMP "f3_server_smoke_doors$phase.log"
        $p = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 3) --data `"$DataDir`" --dev-kit --scripts `"$ScriptsDir`" --saves `"$dsaves`" --run-seconds 90" `
            -RedirectStandardOutput $log4 -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
        try {
            Start-Sleep -Milliseconds 2500
            & $gametest --port ($Port + 3) --doors $phase --state $dstate
            $code = $LASTEXITCODE
        } finally {
            if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
        }
        Write-Host "---- server log (doors, phase $phase) ----"
        Get-Content $log4 | Select-Object -Last 8
        if ($code -ne 0) { $exit = $code; break }
    }
    if (Test-Path $dsaves) { Remove-Item -Recurse -Force $dsaves }
} else {
    Write-Host "gamedata/server not found: real-data test skipped (see docs/server-data.md)"
}
if (Test-Path $saves) { Remove-Item -Recurse -Force $saves }
exit $exit
