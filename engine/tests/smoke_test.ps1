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

function Run-Server([string]$extra, [string]$log, [int]$port) {
    Start-Process -FilePath $server -ArgumentList "--port $port --scripts `"$ScriptsDir`" $extra --run-seconds 90" `
        -RedirectStandardOutput $log -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
}

$exit = 0

# ---- 1. test map
$log1 = Join-Path $env:TEMP "f3_server_smoke.log"
$p = Run-Server "--no-data" $log1 $Port
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
    $p = Run-Server "--data `"$DataDir`"" $log2 ($Port + 1)
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
} else {
    Write-Host "gamedata/server not found: real-data test skipped (see docs/server-data.md)"
}
exit $exit
