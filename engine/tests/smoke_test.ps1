# Starts f3_server, runs f3_testclient against it, then stops the server.
param(
    [string]$BinDir = "$PSScriptRoot\..\..\build\vs2022\bin",
    [string]$ScriptsDir = "$PSScriptRoot\..\..\scripts",
    [int]$Port = 4047
)
$ErrorActionPreference = "Stop"
$server = Join-Path $BinDir "f3_server.exe"
$bot = Join-Path $BinDir "f3_testclient.exe"
$log = Join-Path $env:TEMP "f3_server_smoke.log"
$p = Start-Process -FilePath $server -ArgumentList "--port $Port --scripts `"$ScriptsDir`" --run-seconds 60" `
    -RedirectStandardOutput $log -RedirectStandardInput (New-Item -Force "$env:TEMP\f3_empty_stdin.txt") -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Milliseconds 800
    & $bot --port $Port --name SmokeBot
    $code = $LASTEXITCODE
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
Write-Host "---- server log ----"
Get-Content $log
exit $code
