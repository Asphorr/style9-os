# Launch style9-os in an interactive QEMU window while also mirroring
# every COM1 byte into a host file -- so the assistant can read what
# happened by tailing serial.log instead of asking for screenshots.
#
# The kernel's tty already mirrors to UART (see dev/tty.c + dev/uart.c),
# so kprintf / printf / klog / driver banners ALL land in serial.log.
# Display window stays interactive so you can type into the shell.
#
# Usage:
#   .\tools\run.ps1                          # default kernel.elf + disk.img
#   .\tools\run.ps1 -Disk path\to\other.img  # alternate disk image
#   .\tools\run.ps1 -NoDisk                  # no -hda at all
#   .\tools\run.ps1 -KillExisting            # kill stale qemu first

[CmdletBinding()]
param(
    [string] $Kernel       = 'D:\style9\os\kernel.elf',
    [string] $Disk         = 'D:\style9\os\disk.img',
    [string] $LogFile      = 'D:\style9\os\serial.log',
    [string] $Qemu         = 'C:\Program Files\qemu\qemu-system-x86_64.exe',
    [string] $Cpu          = 'Penryn',  # macOS x86_64 baseline: SSE4.1, no AVX (ring-3 Darwin bins need it; default qemu64 #UDs)
    [switch] $NoDisk,
    [switch] $KillExisting
)

if ($KillExisting) {
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 200
}

if (-not (Test-Path $Kernel)) {
    Write-Error "kernel not built: $Kernel  (run 'make' in WSL first)"
    exit 1
}

# Fresh log each boot.  QEMU opens the file truncating, but explicitly
# removing here means stale bytes from a previous run never linger if
# this script is interrupted before QEMU touches the file.
Remove-Item $LogFile -ErrorAction SilentlyContinue

# QEMU on Windows is happier with forward-slash paths in chardev specs.
$logFwd = $LogFile.Replace('\','/')

$qemuArgs = @(
    '-cpu',     $Cpu,
    '-kernel',  $Kernel,
    '-no-reboot',
    '-serial',  "file:$logFwd"
)
if (-not $NoDisk -and (Test-Path $Disk)) {
    $qemuArgs += @('-hda', $Disk)
}

Write-Host "starting QEMU; serial -> $LogFile"
Start-Process -FilePath $Qemu -ArgumentList $qemuArgs `
    -WorkingDirectory (Split-Path $Kernel)
