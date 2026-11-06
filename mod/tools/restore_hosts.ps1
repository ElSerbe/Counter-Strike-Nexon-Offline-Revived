# Restores the original hosts file from the project backup. Run elevated.
$ErrorActionPreference = "Stop"
$hosts = "$env:SystemRoot\System32\drivers\etc\hosts"
$backup = Join-Path $PSScriptRoot "..\backup\system\hosts.original"
try {
    $item = Get-Item $hosts
    $item.IsReadOnly = $false
    Copy-Item -Path $backup -Destination $hosts -Force
    (Get-Item $hosts).IsReadOnly = $true
    Clear-DnsClientCache
    Write-Output "restored"
    exit 0
} catch {
    Write-Output "error: $($_.Exception.Message)"
    exit 1
}
