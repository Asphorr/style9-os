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
#   .\tools\run.ps1 -MonitorPort 4444        # expose the QEMU monitor on TCP
#
# -MonitorPort opens QEMU's monitor on a local TCP socket so tools\sendkeys.ps1
# can inject real PS/2 keystrokes.  That is the only way to exercise the
# keyboard path without a human at the keyboard: sendkey goes in at the
# controller, through IRQ1, through the driver, through the line discipline --
# the same road a finger takes.

[CmdletBinding()]
param(
    [string] $Kernel       = 'D:\style9\os\kernel.elf',
    [string] $Disk         = 'D:\style9\os\disk.img',
    [string] $LogFile      = 'D:\style9\os\serial.log',
    [string] $Qemu         = 'C:\Program Files\qemu\qemu-system-x86_64.exe',
    [string] $Cpu          = 'Nehalem',  # measured: gls needs SSE4.2 (pcmpgtq), nothing needs AVX -- see QEMU_CPU in the Makefile
    [int]    $MonitorPort  = 0,
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
if ($MonitorPort -gt 0) {
    $qemuArgs += @('-monitor', "tcp:127.0.0.1:$MonitorPort,server,nowait")
}

Write-Host "starting QEMU; serial -> $LogFile"
Start-Process -FilePath $Qemu -ArgumentList $qemuArgs `
    -WorkingDirectory (Split-Path $Kernel)
