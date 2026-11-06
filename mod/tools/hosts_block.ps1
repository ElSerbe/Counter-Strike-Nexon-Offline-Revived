# Adds or removes the CSNZ offline block (tools\hosts_block.txt) in the Windows hosts file, in place: the rest of the
# file is kept as it is. Run elevated (the launcher asks Windows for it). The first time, the hosts file as it was is
# copied to ..\backup\system\hosts.original.
#   powershell -ExecutionPolicy Bypass -File hosts_block.ps1 add
#   powershell -ExecutionPolicy Bypass -File hosts_block.ps1 remove
param([ValidateSet("add", "remove")][string]$Action = "add")
$ErrorActionPreference = "Stop"
$hosts = "$env:SystemRoot\System32\drivers\etc\hosts"
$blockFile = Join-Path $PSScriptRoot "hosts_block.txt"
$backupDir = Join-Path $PSScriptRoot "..\backup\system"
$begin = "# >>> CSNZ offline mod (begin)"
$end = "# <<< CSNZ offline mod (end)"
try {
    $text = if (Test-Path $hosts) { [IO.File]::ReadAllText($hosts) } else { "" }
    New-Item -ItemType Directory -Force $backupDir | Out-Null
    $backup = Join-Path $backupDir "hosts.original"
    if (-not (Test-Path $backup)) { [IO.File]::WriteAllText($backup, $text) }

    # the file without our block (if it was there)
    $lines = $text -split "`r?`n"
    $kept = New-Object System.Collections.Generic.List[string]
    $inside = $false
    foreach ($line in $lines) {
        if ($line.StartsWith($begin)) { $inside = $true; continue }
        if ($inside) { if ($line.StartsWith($end)) { $inside = $false }; continue }
        $kept.Add($line)
    }
    $result = ($kept -join "`r`n").TrimEnd()
    if ($Action -eq "add") {
        $result = $result + "`r`n`r`n" + ([IO.File]::ReadAllText($blockFile).Trim()) + "`r`n"
    } else {
        $result = $result + "`r`n"
    }

    $item = Get-Item $hosts -ErrorAction SilentlyContinue
    $readOnly = $item -and $item.IsReadOnly
    if ($readOnly) { $item.IsReadOnly = $false }
    try {
        [IO.File]::WriteAllText($hosts, $result, [Text.Encoding]::ASCII)
    } finally {
        if ($readOnly) { (Get-Item $hosts).IsReadOnly = $true }
    }
    Clear-DnsClientCache -ErrorAction SilentlyContinue
    Write-Output "hosts block: $Action done"
    exit 0
} catch {
    Write-Output "error: $($_.Exception.Message)"
    exit 1
}
