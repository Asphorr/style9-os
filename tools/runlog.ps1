# Boot the style9-os kernel briefly with QEMU, route debugcon output
# (port 0xE9 -- mirrored by dev/dbgcon.c) into obj/boot.log, then print
# the log.  Equivalent to `make log` but invoked directly from
# PowerShell so it does not depend on WSL's PE-interop being live.
#
# Usage:
#   .\tools\runlog.ps1              # default 2-second capture
#   .\tools\runlog.ps1 -Seconds 5   # longer capture
#   .\tools\runlog.ps1 -Tail        # only show last 25 lines

[CmdletBinding()]
param(
    [int]    $Seconds = 2,
    [string] $Kernel  = 'D:\style9\os\kernel.elf',
    [string] $LogFile = 'D:\style9\os\obj\boot.log',
    [string] $Qemu    = 'C:\Program Files\qemu\qemu-system-x86_64.exe',
    [switch] $Tail
)

# Kill any lingering instance from a previous run so the new boot's
# debugcon owns the file.
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 200

if (-not (Test-Path $Kernel)) {
    Write-Error "kernel not built: $Kernel  (run 'make' in WSL first)"
    exit 1
}

$null = New-Item -ItemType Directory -Force -Path (Split-Path $LogFile)
Remove-Item $LogFile -ErrorAction SilentlyContinue

# QEMU prefers forward-slash paths in chardev specs on Windows.
$logFwd = $LogFile.Replace('\','/')

$proc = Start-Process -FilePath $Qemu -ArgumentList @(
    '-kernel',  $Kernel,
    '-no-reboot',
    '-display', 'none',
    '-serial',  "file:$logFwd"
) -PassThru -NoNewWindow

Start-Sleep -Seconds $Seconds

if (-not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 300

if (-not (Test-Path $LogFile)) {
    Write-Host '(no log produced)'
    exit 1
}

"--- $LogFile (after ${Seconds}s) ---"
if ($Tail) {
    Get-Content $LogFile -Tail 25
} else {
    Get-Content $LogFile
}
'--- end ---'
