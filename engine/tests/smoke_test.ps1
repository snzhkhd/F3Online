# Smoke tests.
#  1. f3_server --no-data (built-in Lua test map) + f3_testclient (headless bot): protocol round trips.
#  0. Data layer: f3_datatest (loader / merge / validation unit tests), f3_server --validate-data on data/ (must pass) and on the
#     deliberately broken engine/tests/data_bad (must fail with a status code, must pass with --lenient).
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
    Start-Process -FilePath $server -ArgumentList "--local --port $port --scripts `"$ScriptsDir`" --saves `"$saves`" $extra --run-seconds 150" `
        -RedirectStandardOutput $log -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
}

$exit = 0

# ---- 0. data layer: unit tests and --validate-data (no network)
$datatest = Join-Path $BinDir "f3_datatest.exe"
if (Test-Path $datatest) {
    & $datatest | Where-Object { $_ -notmatch "INFO" }
    if ($LASTEXITCODE -ne 0) { Write-Host "f3_datatest FAILED"; $exit = 1 }
}
$contentDir = Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) "data"
$badDir = Join-Path $PSScriptRoot "data_bad"
$hasGame = Test-Path (Join-Path $DataDir "manifest.json")
$vdata = if ($hasGame) { @("--data", $DataDir) } else { @("--no-data") }
& $server --validate-data --content $contentDir @vdata
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: --validate-data rejects data/ (exit $LASTEXITCODE)"; $exit = 1 } else { Write-Host "ok:   --validate-data accepts data/" }
$vout = & $server --validate-data --content $badDir @vdata
$vcode = $LASTEXITCODE
$vout | Write-Host
if ($vcode -ne 1) { Write-Host "FAIL: --validate-data on the broken fixture returned $vcode (expected 1)"; $exit = 1 }
elseif (-not ($vout -match "data_bad/perks/perks.json: records\[1\].id: duplicate id") -or -not ($vout -match "unknown perk 'ghost_perk'") -or -not ($vout -match "expected integer, got string") -or -not ($vout -match "unknown field")) { Write-Host "FAIL: the broken fixture's errors are not reported with file and path"; $exit = 1 }
else { Write-Host "ok:   --validate-data fails on the broken fixture (exit 1) and names file + path" }
& $server --validate-data --lenient --content $badDir @vdata | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: --validate-data --lenient should only warn (exit $LASTEXITCODE)"; $exit = 1 } else { Write-Host "ok:   --validate-data --lenient exits 0 on the broken fixture" }
# startup must refuse bad data too (server exits 1 before listening), and --lenient lets it start
$badLog = Join-Path $env:TEMP "f3_server_baddata.log"
$pbad = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 9) --no-data --content `"$badDir`" --scripts `"$ScriptsDir`" --saves `"$saves`" --run-seconds 20" -RedirectStandardOutput $badLog -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
$null = $pbad.Handle   # keep the process handle so ExitCode is available
if (-not $pbad.WaitForExit(15000)) { Stop-Process -Id $pbad.Id -Force; Write-Host "FAIL: server with broken data kept running"; $exit = 1 }
elseif ($pbad.ExitCode -eq 0) { Write-Host "FAIL: server started with broken data"; $exit = 1 }
else { Write-Host "ok:   server refuses to start on broken data (exit $($pbad.ExitCode))" }

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
        $p = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 3) --data `"$DataDir`" --dev-kit --scripts `"$ScriptsDir`" --saves `"$dsaves`" --run-seconds 150" `
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

    # ---- 5. data layer end to end: client sync, perk flow, /reload on a throw-away copy of data/ (the test edits the copy)
    $csaves = Join-Path $env:TEMP ("f3_smoke_content_saves_" + $PID)
    $cdata = Join-Path $env:TEMP ("f3_smoke_content_data_" + $PID)
    foreach ($d in $csaves, $cdata) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
    Copy-Item -Recurse $contentDir $cdata
    $log5 = Join-Path $env:TEMP "f3_server_smoke_content.log"
    $p = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 4) --data `"$DataDir`" --content `"$cdata`" --dev-kit --scripts `"$ScriptsDir`" --saves `"$csaves`" --run-seconds 150" `
        -RedirectStandardOutput $log5 -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 2500
        & $gametest --port ($Port + 4) --content $cdata
        $code = $LASTEXITCODE
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
    Write-Host "---- server log (data layer) ----"
    Get-Content $log5 | Select-Object -Last 25
    if ($code -ne 0) { $exit = $code }
    foreach ($d in $csaves, $cdata) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
} else {
    Write-Host "gamedata/server not found: real-data test skipped (see docs/server-data.md)"
}
if (Test-Path $saves) { Remove-Item -Recurse -Force $saves }
exit $exit
