# Smoke tests.
#  1. f3_server --no-data (built-in Lua test map) + f3_testclient (headless bot): protocol round trips.
#  0c. Localization: f3_localetest (loader, fallbacks, placeholders, UTF-8 wrap), f3_server --check-locale --strict on data/locale (+ a broken fixture
#      that must fail); with game data: f3_gametest --locale (per-player language of server messages, Cyrillic names / chat).
#  0. Data layer: f3_datatest (loader / merge / validation unit tests), f3_server --validate-data on data/ (must pass) and on the
#     deliberately broken engine/tests/data_bad (must fail with a status code, must pass with --lenient).
#  0d. Content editor (--editor): f3_editortest, then a live server on a copy of data/: token / Host / path-confinement rejections, get / put / validate / reload.
#  2b. NPC life (respawn, leash / home, UseSkill approach): f3_gametest --npclife against a --dev-kit server.
#  5c. Object scripts (data/objects + scripts/objects): f3_gametest --objects 1|2 with a server restart in between.
#  2. If gamedata/server exists (run f3_dataimport first): f3_server on the real data + f3_gametest
#     (real map grid, stats, inventory, walk, pickup, combat, map transition, door). Skipped otherwise.
param(
    [string]$BinDir = "$PSScriptRoot\..\..\build\vs2022\bin",
    [string]$ScriptsDir = "$PSScriptRoot\..\..\scripts",
    [string]$DataDir = "$PSScriptRoot\..\..\gamedata\server",
    [int]$Port = 4047,
    [uint32]$Seed = 0          # server RNG seed (combat / lock rolls); 0 = pick one and print it, rerun with -Seed N to replay a failure
)
$ErrorActionPreference = "Stop"
$server = Join-Path $BinDir "f3_server.exe"
$bot = Join-Path $BinDir "f3_testclient.exe"
$gametest = Join-Path $BinDir "f3_gametest.exe"
$empty = New-Item -Force "$env:TEMP\f3_empty_stdin.txt"

$saves = Join-Path $env:TEMP ("f3_smoke_saves_" + $PID)   # throw-away account directory, never the real saves/
if (Test-Path $saves) { Remove-Item -Recurse -Force $saves }

function Run-Server([string]$extra, [string]$log, [int]$port) {
    Start-Process -FilePath $server -ArgumentList "--local --port $port --scripts `"$ScriptsDir`" --saves `"$saves`" --seed $Seed $extra --run-seconds 150" `
        -RedirectStandardOutput $log -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
}

# Runs f3_gametest with the given arguments, echoes its output and, on failure, lists the FAILED check names (and the
# bot diagnostics) once more at the end so they are not lost in a long log. Returns the exit code.
function Run-GameTest([string[]]$gtArgs) {
    $out = @(& $gametest @gtArgs)
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }
    if ($code -ne 0) {
        Write-Host "==== f3_gametest $($gtArgs -join ' ') FAILED (exit $code, server seed $Seed): failing checks ===="
        $out | Where-Object { $_ -match '^FAIL' } | ForEach-Object { Write-Host "  $_" }
        if (-not ($out | Where-Object { $_ -match '^FAIL' })) { Write-Host "  (no FAIL line: the test aborted early, see the output above)" }
    }
    return $code
}

$exit = 0

# ---- 0. data layer: unit tests and --validate-data (no network)
$datatest = Join-Path $BinDir "f3_datatest.exe"
if (Test-Path $datatest) {
    & $datatest | Where-Object { $_ -notmatch "INFO" }
    if ($LASTEXITCODE -ne 0) { Write-Host "f3_datatest FAILED"; $exit = 1 }
}
# ---- 0b. persistence: unit tests (SQLite store, JSON import / export, corrupt files) + the server's database tools and first-start import
$dbtest = Join-Path $BinDir "f3_dbtest.exe"
if (Test-Path $dbtest) {
    & $dbtest | Where-Object { $_ -notmatch "INFO|WARN|ERROR" }
    if ($LASTEXITCODE -ne 0) { Write-Host "f3_dbtest FAILED"; $exit = 1 }
}
$dbs = Join-Path $env:TEMP ("f3_smoke_db_" + $PID)
if (Test-Path $dbs) { Remove-Item -Recurse -Force $dbs }
New-Item -ItemType Directory -Force (Join-Path $dbs "accounts") | Out-Null
$salt = [byte[]](1..16)
$kdf = New-Object System.Security.Cryptography.Rfc2898DeriveBytes("oldpass", $salt, 1000, [System.Security.Cryptography.HashAlgorithmName]::SHA256)
$hashHex = -join ($kdf.GetBytes(32) | ForEach-Object { $_.ToString("x2") })
$saltHex = -join ($salt | ForEach-Object { $_.ToString("x2") })
$acc = '{"name":"OldUser","algo":"pbkdf2-sha256","iterations":1000,"salt":"' + $saltHex + '","hash":"' + $hashHex + '","characters":[{"name":"OldUser","level":3,"xp":500,"map":"town","x":5,"y":6,"created":true,"quests":[{"id":"q","title":"T","text":"x","state":0,"updated":1}],"friends":["Pal"]}]}'
[System.IO.File]::WriteAllText((Join-Path $dbs "accounts\acc_olduser.json"), $acc)
$mout = & $server --saves $dbs --migrate-json
if ($LASTEXITCODE -ne 0 -or -not ($mout -match "imported 1 account") -or -not (Test-Path (Join-Path $dbs "accounts.migrated\acc_olduser.json")) -or (Test-Path (Join-Path $dbs "accounts"))) { Write-Host "FAIL: --migrate-json"; $mout | Write-Host; $exit = 1 } else { Write-Host "ok:   --migrate-json imports the old JSON saves and keeps them as accounts.migrated" }
$mout = & $server --saves $dbs --migrate-json
if ($LASTEXITCODE -ne 0 -or -not ($mout -match "nothing to migrate")) { Write-Host "FAIL: --migrate-json is not idempotent"; $exit = 1 } else { Write-Host "ok:   --migrate-json twice: second run does nothing" }
$dump = (& $server --saves $dbs --db-dump) -join "`n"
if ($LASTEXITCODE -ne 0 -or -not ($dump -match "OldUser") -or ($dump -match $hashHex) -or -not ($dump -match '"friend":"Pal"')) { Write-Host "FAIL: --db-dump"; $exit = 1 } else { Write-Host "ok:   --db-dump shows the imported rows and hides the hash" }
& $server --saves $dbs --export-json (Join-Path $dbs "export") | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $dbs "export\accounts\acc_olduser.json"))) { Write-Host "FAIL: --export-json"; $exit = 1 } else { Write-Host "ok:   --export-json writes the old format" }
# server start on a damaged database: refuses (exit 1) and leaves the file alone
$bsaves = Join-Path $env:TEMP ("f3_smoke_baddb_" + $PID)
if (Test-Path $bsaves) { Remove-Item -Recurse -Force $bsaves }
New-Item -ItemType Directory -Force $bsaves | Out-Null
[System.IO.File]::WriteAllText((Join-Path $bsaves "f3.db"), "this is not a database, do not touch")
$bdlog = Join-Path $env:TEMP "f3_server_baddb.log"
$pdb = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 8) --no-data --scripts `"$ScriptsDir`" --saves `"$bsaves`" --run-seconds 20" -RedirectStandardOutput $bdlog -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
$null = $pdb.Handle
if (-not $pdb.WaitForExit(15000)) { Stop-Process -Id $pdb.Id -Force; Write-Host "FAIL: server kept running on a damaged database"; $exit = 1 }
elseif ($pdb.ExitCode -eq 0 -or -not ((Get-Content $bdlog -Raw) -match "NOT modified") -or ([System.IO.File]::ReadAllText((Join-Path $bsaves "f3.db")) -ne "this is not a database, do not touch")) { Write-Host "FAIL: damaged database handling (exit $($pdb.ExitCode))"; $exit = 1 }
else { Write-Host "ok:   server refuses a damaged database with a clear error and leaves the file untouched" }
Remove-Item -Recurse -Force $bsaves
# first start on old JSON saves: imported automatically
New-Item -ItemType Directory -Force (Join-Path $dbs "fs\accounts") | Out-Null
Copy-Item (Join-Path $dbs "accounts.migrated\acc_olduser.json") (Join-Path $dbs "fs\accounts\acc_olduser.json")
$fslog = Join-Path $env:TEMP "f3_server_firststart.log"
$pfs = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 7) --no-data --scripts `"$ScriptsDir`" --saves `"$(Join-Path $dbs 'fs')`" --run-seconds 4" -RedirectStandardOutput $fslog -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
$null = $pfs.Handle
if (-not $pfs.WaitForExit(20000)) { Stop-Process -Id $pfs.Id -Force }
if (-not ((Get-Content $fslog -Raw) -match "1 account\(s\)") -or -not (Test-Path (Join-Path $dbs "fs\accounts.migrated")) -or -not (Test-Path (Join-Path $dbs "fs\f3.db"))) { Write-Host "FAIL: automatic import at first start"; Get-Content $fslog | Write-Host; $exit = 1 } else { Write-Host "ok:   first start imports the old JSON saves automatically" }
Remove-Item -Recurse -Force $dbs

$contentDir = Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) "data"
$badDir = Join-Path $PSScriptRoot "data_bad"
$hasGame = Test-Path (Join-Path $DataDir "manifest.json")
$vdata = if ($hasGame) { @("--data", $DataDir) } else { @("--no-data") }
& $server --validate-data --content $contentDir @vdata
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: --validate-data rejects data/ (exit $LASTEXITCODE)"; $exit = 1 } else { Write-Host "ok:   --validate-data accepts data/" }
if ($hasGame) {
    & $server --export-data --check --content $contentDir --data $DataDir | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: --export-data --check: the exported data/ files differ from the original data (exit $LASTEXITCODE)"; $exit = 1 } else { Write-Host "ok:   data/ exports are up to date with the original data" }
}
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

# ---- 0c. localization: unit tests (loader, fallbacks, placeholders, UTF-8 wrap) and the coverage report of data/locale
$localetest = Join-Path $BinDir "f3_localetest.exe"
if (Test-Path $localetest) {
    & $localetest | Where-Object { $_ -notmatch "INFO" }
    if ($LASTEXITCODE -ne 0) { Write-Host "f3_localetest FAILED"; $exit = 1 }
}
$lout = & $server --check-locale --content $contentDir --strict
if ($LASTEXITCODE -ne 0) { Write-Host ($lout -join "`n"); Write-Host "FAIL: --check-locale reports problems in data/locale"; $exit = 1 }
else { Write-Host "ok:   --check-locale: every key translated, placeholders match, no undefined keys"; $lout | Select-String "^language|locale check" | ForEach-Object { Write-Host "      $_" } }
$badLoc = Join-Path $env:TEMP ("f3_smoke_badlocale_" + $PID)   # a broken locale folder: placeholder mismatch + missing key + used-but-undefined key
if (Test-Path $badLoc) { Remove-Item -Recurse -Force $badLoc }
foreach ($l in "en", "ru", "src") { New-Item -ItemType Directory -Force (Join-Path $badLoc $l) | Out-Null }
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $badLoc "en\ui.json"), '{"ui.a":"A {n}","ui.b":"B"}', $utf8)
[IO.File]::WriteAllText((Join-Path $badLoc "ru\ui.json"), '{"ui.a":"A {m}"}', $utf8)
[IO.File]::WriteAllText((Join-Path $badLoc "src\code.cpp"), 'const char* k = "ui.a"; const char* u = "ui.undefined_key";', $utf8)
$blout = & $server --check-locale --content $contentDir --locale $badLoc --src (Join-Path $badLoc "src")
if ($LASTEXITCODE -ne 1 -or -not (($blout -join "`n") -match "ui.undefined_key" -and ($blout -join "`n") -match "placeholders differ")) { Write-Host ($blout -join "`n"); Write-Host "FAIL: --check-locale did not fail on the broken fixture (exit $LASTEXITCODE)"; $exit = 1 }
else { Write-Host "ok:   --check-locale fails on a broken locale (placeholder mismatch, undefined key)" }
Remove-Item -Recurse -Force $badLoc

# ---- 0d. content editor (f3_server --editor): API unit tests, then a live server on a throw-away copy of data/
$editortest = Join-Path $BinDir "f3_editortest.exe"
if (Test-Path $editortest) {
    & $editortest | Where-Object { $_ -notmatch "INFO" }
    if ($LASTEXITCODE -ne 0) { Write-Host "f3_editortest FAILED"; $exit = 1 }
}
$edPort = Get-Random -Minimum 47500 -Maximum 47990
$edRoot = Join-Path $env:TEMP ("f3_smoke_editor_" + $PID)
if (Test-Path $edRoot) { Remove-Item -Recurse -Force $edRoot }
New-Item -ItemType Directory -Force $edRoot | Out-Null
Copy-Item -Recurse $contentDir (Join-Path $edRoot "data")
$edData = Join-Path $edRoot "data"
$edUi = Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) "tools\editor"
function Test-PortOpen([int]$port) {
    $c = New-Object System.Net.Sockets.TcpClient
    try { $c.Connect("127.0.0.1", $port); return $true } catch { return $false } finally { $c.Close() }
}
function Ed-Call([string]$method, [string]$path, $body, [string]$token) {   # returns @{status; json}
    $h = @{}
    if ($token) { $h["X-F3-Token"] = $token }
    $a2 = @{ Uri = "http://127.0.0.1:$edPort/api/$path"; Method = $method; Headers = $h; UseBasicParsing = $true; ErrorAction = "Stop" }
    if ($null -ne $body) { $a2["Body"] = [Text.Encoding]::UTF8.GetBytes((ConvertTo-Json -InputObject $body -Depth 40 -Compress)); $a2["ContentType"] = "application/json" }
    try { $r = Invoke-WebRequest @a2; return @{ status = [int]$r.StatusCode; json = ($r.Content | ConvertFrom-Json) } }
    catch {
        $resp = $_.Exception.Response
        if ($null -eq $resp) { return @{ status = 0; json = $null } }
        $txt = ""
        if ($_.ErrorDetails -and $_.ErrorDetails.Message) { $txt = $_.ErrorDetails.Message }
        else { try { $sr = New-Object IO.StreamReader($resp.GetResponseStream()); $txt = $sr.ReadToEnd() } catch {} }
        $j = $null; try { $j = $txt | ConvertFrom-Json } catch {}
        return @{ status = [int]$resp.StatusCode; json = $j }
    }
}
function Ed-Check([bool]$cond, [string]$what) {
    if ($cond) { Write-Host "ok:   editor: $what" } else { Write-Host "FAIL: editor: $what"; $script:exit = 1 }
}
# --local alone must not open the editor; a non-loopback editor bind must be refused (exit 2) before anything starts
$pLocal = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 21) --no-data --content `"$edData`" --scripts `"$ScriptsDir`" --saves `"$saves`" --editor-port $edPort --run-seconds 6" -RedirectStandardOutput (Join-Path $edRoot "local.log") -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 2500
Ed-Check (-not (Test-PortOpen $edPort)) "--local alone does not open the editor port"
if (-not $pLocal.HasExited) { Stop-Process -Id $pLocal.Id -Force }
$pBind = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 22) --no-data --content `"$edData`" --scripts `"$ScriptsDir`" --saves `"$saves`" --editor --editor-port $edPort --editor-bind 0.0.0.0 --run-seconds 10" -RedirectStandardOutput (Join-Path $edRoot "bind.log") -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
$null = $pBind.Handle
if (-not $pBind.WaitForExit(10000)) { Stop-Process -Id $pBind.Id -Force; Ed-Check $false "--editor-bind 0.0.0.0 is refused" }
else { Ed-Check ($pBind.ExitCode -eq 2) "--editor-bind 0.0.0.0 is refused (exit $($pBind.ExitCode))" }
$edLog = Join-Path $edRoot "editor.log"
$pEd = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 23) --no-data --content `"$edData`" --scripts `"$ScriptsDir`" --saves `"$saves`" --editor --editor-port $edPort --editor-dir `"$edUi`" --run-seconds 90" -RedirectStandardOutput $edLog -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
try {
    $tok = $null
    for ($i = 0; $i -lt 60 -and -not $tok; $i++) {
        Start-Sleep -Milliseconds 250
        if (Test-Path $edLog) { $m = [regex]::Match([string](Get-Content $edLog -Raw -ErrorAction SilentlyContinue), "token=([0-9a-f]{32})"); if ($m.Success) { $tok = $m.Groups[1].Value } }
    }
    Ed-Check ($null -ne $tok) "server prints the access token"
    if ($tok) {
        Ed-Check ((Ed-Call "GET" "kinds" $null $null).status -eq 401) "no token is rejected (401)"
        Ed-Check ((Ed-Call "GET" "kinds" $null ("0" * 32)).status -eq 401) "wrong token is rejected (401)"
        $k = Ed-Call "GET" "kinds" $null $tok
        Ed-Check ($k.status -eq 200 -and $k.json.kinds.Count -ge 7) "GET /api/kinds with the token"
        $sc = Ed-Call "GET" "schema" $null $tok
        Ed-Check ($sc.status -eq 200 -and $null -ne $sc.json.kinds.items) "GET /api/schema (generated from the validator's definitions)"
        $l = Ed-Call "GET" "list?kind=quests" $null $tok
        Ed-Check ($l.status -eq 200 -and $l.json.rows.Count -ge 1) "GET /api/list?kind=quests"
        $g = Ed-Call "GET" "get?kind=quests&file=quests.json" $null $tok
        Ed-Check ($g.status -eq 200 -and $g.json.parsed -and $g.json.etag) "GET /api/get returns the file and an etag"
        $v = Ed-Call "POST" "validate" @{} $tok
        Ed-Check ($v.status -eq 200 -and $v.json.ok) "POST /api/validate on the clean copy"
        # PUT a changed title; the file on disk must carry it
        $doc = $g.json.doc
        $doc.records[0].title = "Smoke edited title"
        $pu = Ed-Call "PUT" "put" @{ kind = "quests"; file = "quests.json"; doc = $doc; etag = $g.json.etag } $tok
        Ed-Check ($pu.status -eq 200 -and $pu.json.ok) "PUT /api/put saves a valid edit"
        $disk = Get-Content (Join-Path $edData "quests\quests.json") -Raw
        Ed-Check ($disk -match "Smoke edited title") "the saved file contains the edit"
        # stale etag -> 409; invalid -> 422 and nothing written
        $st = Ed-Call "PUT" "put" @{ kind = "quests"; file = "quests.json"; doc = $doc; etag = $g.json.etag } $tok
        Ed-Check ($st.status -eq 409) "a stale etag is a conflict (409)"
        $g2 = Ed-Call "GET" "get?kind=quests&file=quests.json" $null $tok
        $bad = $g2.json.doc
        $bad.records[0] | Add-Member -NotePropertyName "no_such_field" -NotePropertyValue 1
        $before = Get-Content (Join-Path $edData "quests\quests.json") -Raw
        $pb = Ed-Call "PUT" "put" @{ kind = "quests"; file = "quests.json"; doc = $bad; etag = $g2.json.etag } $tok
        $after = Get-Content (Join-Path $edData "quests\quests.json") -Raw
        Ed-Check ($pb.status -eq 422 -and $pb.json.errors.Count -ge 1) "an invalid edit is refused (422) with errors"
        Ed-Check ($before -eq $after) "the refused edit did not touch the file"
        # reload: the running server re-reads data/ and reports the diff
        $rl = Ed-Call "POST" "reload" @{} $tok
        Ed-Check ($rl.status -eq 200 -and $rl.json.ok -and ($rl.json.report -match "data reloaded")) "POST /api/reload applies the change and returns the report"
        # confinement
        foreach ($t in @("get?kind=quests&file=..%2f..%2fmanifest.json", "get?kind=quests&file=..%5c..%5cmanifest.json", "get?kind=..%2f&file=quests.json", "get?kind=quests&file=C%3A%5CWindows%5Cwin.ini", "get?kind=quests&file=quests.txt", "get?kind=locale%2f..&file=x.json")) {
            $r = Ed-Call "GET" $t $null $tok
            Ed-Check ($r.status -ge 400 -and $r.status -lt 500) "path escape '$t' is rejected ($($r.status))"
        }
        $tr = Ed-Call "PUT" "put" @{ kind = "quests"; file = "..\escape.json"; doc = @{ schema = 1; kind = "quests"; records = @() } } $tok
        Ed-Check ($tr.status -ge 400 -and -not (Test-Path (Join-Path $edData "escape.json")) -and -not (Test-Path (Join-Path $edRoot "escape.json"))) "PUT with a traversal file name is rejected and writes nothing"
        # a foreign Host header (DNS rebinding) is refused even with the right token
        $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $edPort)
        try {
            $ns = $c.GetStream()
            $req = "GET /api/kinds HTTP/1.1`r`nHost: evil.example.com`r`nX-F3-Token: $tok`r`nConnection: close`r`n`r`n"
            $bytes = [Text.Encoding]::ASCII.GetBytes($req)
            $ns.Write($bytes, 0, $bytes.Length)
            $buf = New-Object byte[] 4096
            $n = $ns.Read($buf, 0, $buf.Length)
            $first = [Text.Encoding]::ASCII.GetString($buf, 0, [Math]::Max($n, 0)).Split("`n")[0]
            Ed-Check ($first -match " 403 ") "a foreign Host header is refused (403): $($first.Trim())"
        } finally { $c.Close() }
        # the UI is served; the token in the URL is accepted for the page only
        $ix = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$edPort/?token=$tok"
        Ed-Check ($ix.StatusCode -eq 200 -and $ix.Content -match "F3 Content Editor") "GET / serves the editor page"
        $ixBad = 0; try { Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$edPort/api/kinds?token=$tok" -ErrorAction Stop | Out-Null; $ixBad = 200 } catch { $ixBad = [int]$_.Exception.Response.StatusCode }
        Ed-Check ($ixBad -eq 401) "the query token is not accepted for /api"
    }
} finally {
    if (-not $pEd.HasExited) { Stop-Process -Id $pEd.Id -Force }
    Remove-Item -Recurse -Force $edRoot -ErrorAction SilentlyContinue
}

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
        $code = Run-GameTest @("--port", ($Port + 1))
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
    Write-Host "---- server log (game data) ----"
    Get-Content $log2
    if ($code -ne 0) { $exit = $code }

    # ---- 2b. NPC life + skill approach: respawn (timer / 0 = never / waits for players), leash and walk home, empty map reset,
    #          CleanupJunktown no longer deletes NPCs, UseSkill on a far target walks there, Esc cancels (uses the dev-only /npcdev command)
    $logL = Join-Path $env:TEMP "f3_server_smoke_npclife.log"
    $p = Run-Server "--data `"$DataDir`" --dev-kit" $logL ($Port + 14)
    try {
        Start-Sleep -Milliseconds 2500
        $code = Run-GameTest @("--port", ($Port + 14), "--npclife")
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
    if ($code -ne 0) { Write-Host "---- server log (npclife) ----"; Get-Content $logL; $exit = $code }

    # ---- 3. accounts + persistence: register, play, quit; RESTART the server; login and check everything came back
    $state = Join-Path $env:TEMP "f3_acct_state.txt"
    foreach ($phase in 1, 2) {
        $log3 = Join-Path $env:TEMP "f3_server_smoke_acct$phase.log"
        $p = Run-Server "--data `"$DataDir`"" $log3 ($Port + 2)
        try {
            Start-Sleep -Milliseconds 2500
            $code = Run-GameTest @("--port", ($Port + 2), "--accounts", $phase, "--state", $state)
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
        $p = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 3) --data `"$DataDir`" --dev-kit --scripts `"$ScriptsDir`" --saves `"$dsaves`" --seed $Seed --run-seconds 150" `
            -RedirectStandardOutput $log4 -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
        try {
            Start-Sleep -Milliseconds 2500
            $code = Run-GameTest @("--port", ($Port + 3), "--doors", $phase, "--state", $dstate)
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
    # the test overrides records that the exporter also writes (Autoloader, Bobby): ids must be unique in data/, so the copy drops the generated files
    foreach ($g in "items\weapons.json", "npcs\original.json") { Remove-Item -Force (Join-Path $cdata $g) -ErrorAction SilentlyContinue }
    $log5 = Join-Path $env:TEMP "f3_server_smoke_content.log"
    $p = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 4) --data `"$DataDir`" --content `"$cdata`" --dev-kit --scripts `"$ScriptsDir`" --saves `"$csaves`" --seed $Seed --run-seconds 150" `
        -RedirectStandardOutput $log5 -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 2500
        $code = Run-GameTest @("--port", ($Port + 4), "--content", $cdata)
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
    Write-Host "---- server log (data layer) ----"
    Get-Content $log5 | Select-Object -Last 25
    if ($code -ne 0) { $exit = $code }
    foreach ($d in $csaves, $cdata) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }

    # ---- 5b. data dialogs end to end (data/dialogs): conditions, effects, quest/flag state on a throw-away copy of data/
    $dsaves2 = Join-Path $env:TEMP ("f3_smoke_dlg_saves_" + $PID)
    $ddata2 = Join-Path $env:TEMP ("f3_smoke_dlg_data_" + $PID)
    foreach ($d in $dsaves2, $ddata2) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
    Copy-Item -Recurse $contentDir $ddata2
    $log5b = Join-Path $env:TEMP "f3_server_smoke_dialogs.log"
    $p = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 12) --data `"$DataDir`" --content `"$ddata2`" --dev-kit --scripts `"$ScriptsDir`" --saves `"$dsaves2`" --run-seconds 100" `
        -RedirectStandardOutput $log5b -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 2500
        & $gametest --port ($Port + 12) --dialogs
        $code = $LASTEXITCODE
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
    Write-Host "---- server log (data dialogs) ----"
    Get-Content $log5b | Select-Object -Last 8
    if ($code -ne 0) { $exit = $code }
    foreach ($d in $dsaves2, $ddata2) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }

    # ---- 5c. object scripts end to end (data/objects, scripts/objects): the sample bindings (renamed from _samples.json) plus the declarative test bindings
    # on a throw-away copy of data/; two phases with a server RESTART in between (one-shot marks and object flags persist in the SQLite world state)
    $osaves = Join-Path $env:TEMP ("f3_smoke_obj_saves_" + $PID)
    $odata = Join-Path $env:TEMP ("f3_smoke_obj_data_" + $PID)
    foreach ($d in $osaves, $odata) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
    Copy-Item -Recurse $contentDir $odata
    Copy-Item (Join-Path $odata "objects\_samples.json") (Join-Path $odata "objects\samples.json")
    Copy-Item (Join-Path $PSScriptRoot "objdata\test_objects.json") (Join-Path $odata "objects\test_objects.json")
    foreach ($phase in 1, 2) {
        $log5c = Join-Path $env:TEMP "f3_server_smoke_objects$phase.log"
        $p = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 13) --data `"$DataDir`" --content `"$odata`" --dev-kit --scripts `"$ScriptsDir`" --saves `"$osaves`" --seed $Seed --run-seconds 120" `
            -RedirectStandardOutput $log5c -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
        try {
            Start-Sleep -Milliseconds 2500
            $code = Run-GameTest @("--port", ($Port + 13), "--objects", $phase)
        } finally {
            if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
        }
        Write-Host "---- server log (object scripts, phase $phase) ----"
        Get-Content $log5c | Select-Object -Last 8
        if ($code -ne 0) { $exit = $code; break }
    }
    foreach ($d in $osaves, $odata) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }

    # ---- 6. localization end to end: the language each player announces at login selects the language of server messages and
    # of the data names (perks / traits); /lang switches at run time; UTF-8 (Cyrillic) names and chat are validated in code points
    $lsaves = Join-Path $env:TEMP ("f3_smoke_locale_saves_" + $PID)
    if (Test-Path $lsaves) { Remove-Item -Recurse -Force $lsaves }
    $log6 = Join-Path $env:TEMP "f3_server_smoke_locale.log"
    $p = Start-Process -FilePath $server -ArgumentList "--local --port $($Port + 5) --data `"$DataDir`" --dev-kit --scripts `"$ScriptsDir`" --saves `"$lsaves`" --seed $Seed --run-seconds 120" `
        -RedirectStandardOutput $log6 -RedirectStandardInput $empty -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 2500
        $code = Run-GameTest @("--port", ($Port + 5), "--locale")
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
    if ($code -ne 0) { Write-Host "---- server log (localization) ----"; Get-Content $log6 | Select-Object -Last 25; $exit = $code }
    Start-Sleep -Milliseconds 800   # let the killed server release its database files
    if (Test-Path $lsaves) { Remove-Item -Recurse -Force $lsaves -ErrorAction SilentlyContinue }
} else {
    Write-Host "gamedata/server not found: real-data test skipped (see docs/server-data.md)"
}
if (Test-Path $saves) { Remove-Item -Recurse -Force $saves }
exit $exit
