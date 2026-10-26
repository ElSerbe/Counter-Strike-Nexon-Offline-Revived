# CSNZ offline launcher: one click starts CSNZ_Server, makes sure the client patches are applied,
# launches the game, and stops the server it started once the game is closed.
$ErrorActionPreference = "Stop"

$Mod = $PSScriptRoot
$GameBin = "D:\SteamLibrary\steamapps\common\CSNZ\Bin"
$LogDir = Join-Path $Mod "logs"
$LauncherLog = Join-Path $LogDir "launcher.log"
New-Item -ItemType Directory -Force $LogDir | Out-Null

Add-Type -AssemblyName System.Windows.Forms

function Write-Log($msg) {
    Add-Content -Path $LauncherLog -Value ("{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $msg)
}

function Show-Error($msg) {
    Write-Log "ERROR: $msg"
    [System.Windows.Forms.MessageBox]::Show($msg, "CSNZ Offline", "OK", "Error") | Out-Null
}

function Test-Listening($port) {
    [bool](Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue)
}

function Wait-Listening($port, $seconds) {
    for ($i = 0; $i -lt $seconds * 2; $i++) {
        if (Test-Listening $port) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Stop-Server {
    Get-Process -Name CSNZ_Server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Get-CimInstance Win32_Process | Where-Object {
        ($_.Name -eq "cmd.exe" -and $_.CommandLine -match "run_server") -or
        ($_.Name -eq "powershell.exe" -and $_.CommandLine -match "Start-Sleep -Seconds 999999")
    } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

Write-Log "---- launch"
$started = @{ Server = $false }

try {
    if (Get-Process -Name "cstrike-online" -ErrorAction SilentlyContinue) {
        Show-Error "El juego ya está abierto."
        exit 1
    }

    $hosts = Get-Content "$env:SystemRoot\System32\drivers\etc\hosts" -Raw
    if ($hosts -notmatch "CSNZ offline mod \(begin\)") {
        Show-Error "Falta el bloque de CSNZ en el archivo hosts. Ejecuta mod\tools\add_hosts_block.ps1 como administrador."
        exit 1
    }

    $python = (& py -3 -c "import sys; print(sys.executable)").Trim()

    # Re-apply client patches (idempotent; aborts if the files changed, e.g. after a Steam update).
    $patchOut = & $python (Join-Path $Mod "tools\apply_patches.py") $GameBin apply 2>&1
    $patchOut | ForEach-Object { Write-Log "patch: $_" }
    if ($LASTEXITCODE -ne 0) {
        Show-Error "No se pudieron aplicar los parches del cliente (¿actualizó Steam el juego?). Revisa mod\logs\launcher.log."
        exit 1
    }

    # The web stubs (mod\stubs) are no longer started: with nxgsm.dll and CrashReporter.dll patched the game
    # sends no HTTP requests, and any leftover one hits the hosts block (127.0.0.1) and fails at once.

    if (Test-Listening 30002) {
        Write-Log "CSNZ_Server already listening, reusing it"
    } else {
        Start-Process -FilePath "cmd.exe" -ArgumentList "/c", "`"$(Join-Path $Mod "server\run_server.cmd")`"" -WindowStyle Hidden
        $started.Server = $true
        if (-not (Wait-Listening 30002 30)) { throw "CSNZ_Server no arrancó en el puerto 30002. Revisa mod\server\server.log." }
        Write-Log "started CSNZ_Server"
    }

    try {
        # The in-game browser (CEF 109) reads Chromium switches from the process command line: send all its
        # traffic to a closed local port so it can't reach the internet (Chromium never proxies localhost,
        # so pages served by our own local server still load).
        Start-Process -FilePath (Join-Path $GameBin "cstrike-online.exe") -WorkingDirectory $GameBin -Verb RunAs `
            -ArgumentList "--proxy-server=http://127.0.0.1:9"
    } catch {
        throw "No se pudo abrir el juego (¿se canceló el aviso de administrador?)."
    }
    $launchTime = Get-Date
    Write-Log "game launched"

    # Elevated process: poll by name instead of waiting on a handle. While it runs, log every TCP connection
    # the game (or its CEF helpers) makes to a non-local address in logs\network.log: it should stay empty.
    $networkLog = Join-Path $LogDir "network.log"
    $seen = @{}
    for ($i = 0; $i -lt 60 -and -not (Get-Process -Name "cstrike-online" -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Seconds 1 }
    while ($game = @(Get-Process -Name "cstrike-online" -ErrorAction SilentlyContinue)) {
        $pids = @($game.Id) + @(Get-Process -Name "CefSubProcess" -ErrorAction SilentlyContinue | ForEach-Object Id)
        Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object {
            $pids -contains $_.OwningProcess -and $_.RemoteAddress -notmatch '^(127\.|0\.0\.0\.0$|::1?$)'
        } | ForEach-Object {
            $key = "$($_.RemoteAddress):$($_.RemotePort)"
            if (-not $seen.ContainsKey($key)) {
                $seen[$key] = $true
                # Name it right away from the DNS cache (short TTLs expire within seconds).
                $ip = $_.RemoteAddress
                $names = @(Get-DnsClientCache -ErrorAction SilentlyContinue | Where-Object { $_.Data -eq $ip } |
                    ForEach-Object Entry | Select-Object -Unique) -join ", "
                $proc = (Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue).Name
                Add-Content -Path $networkLog -Value ("{0} {1} (pid {2}) -> {3} ({4}) {5}" -f
                    (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $proc, $_.OwningProcess, $key, $_.State, $names)
            }
        }
        Start-Sleep -Seconds 1
    }
    Write-Log "game closed ($($seen.Count) external connection(s), see logs\network.log)"

    # Windows Error Reporting leaves a dump here when the game crashes, even on exit (no visible error).
    $dumps = @(Get-ChildItem "$env:LOCALAPPDATA\CrashDumps\cstrike-online.exe.*.dmp" -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -gt $launchTime })
    if ($dumps) { Write-Log "WARNING: the game crashed, dump: $($dumps[-1].FullName)" }
}
catch {
    Show-Error $_.Exception.Message
}
finally {
    if ($started.Server) {
        Start-Sleep -Seconds 5  # let the server finish saving the account after the client disconnects
        Stop-Server
    }
    Write-Log "stopped what this launcher started"
}
