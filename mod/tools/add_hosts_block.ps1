# Rebuilds the hosts file as: clean backup + current CSNZ offline block. Run elevated.
# Builds the full content in a temp file, then copies it over the hosts file.
# The hosts file is read-only by default; the flag is restored afterwards.
$ErrorActionPreference = "Stop"
$hosts = "$env:SystemRoot\System32\drivers\etc\hosts"
$backup = Join-Path $PSScriptRoot "..\backup\system\hosts.original"
$block = Join-Path $PSScriptRoot "hosts_block.txt"
$tmp = Join-Path $env:TEMP "csnz_hosts_new.txt"
try {
    $base = Get-Content -Raw -Path $backup
    $text = Get-Content -Raw -Path $block
    Set-Content -Path $tmp -Value ($base.TrimEnd() + "`r`n`r`n" + $text) -Encoding ascii
    if ((Get-Item $tmp).Length -lt 100000) { throw "combined hosts file unexpectedly small" }
    (Get-Item $hosts).IsReadOnly = $false
    try {
        Copy-Item -Path $tmp -Destination $hosts -Force
    } finally {
        (Get-Item $hosts).IsReadOnly = $true
    }
    Clear-DnsClientCache
    Write-Output "rebuilt"
    exit 0
} catch {
    "error: $($_.Exception.Message) at $($_.InvocationInfo.ScriptLineNumber)" | Out-File -FilePath (Join-Path $PSScriptRoot "hosts_sync.log") -Append -Encoding ascii
    exit 1
}
