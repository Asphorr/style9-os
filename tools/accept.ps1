# Acceptance harness for the APFS writer: N boots in a row, apfsck after every
# one of them, and a tally of what each boot said.
#
#   .\tools\accept.ps1 -Boots 6 [-Fresh] [-TimeoutSec 200]
#
# -Fresh regenerates obj/style9.apfs from the fixture first, so boot 1 starts
# from the pristine volume the way a first boot on an image does.
#
# THE DISK IS NOT OPTIONAL.  A QEMU that came up without one runs a clean
# looking set of tests against nothing at all, which reads as a pass.  That has
# happened twice, so this checks the kernel found a drive and throws if it did
# not.
#
# ...AND THE CHECK ITSELF READ THE WRONG WAY ROUND FOR A WHILE.  It matched
# /ata: .*drive/, which the failure line "ata: no drives detected" satisfies as
# happily as the success line does, so the one boot in this run that came up
# with no disk sailed past the guard and was tallied at 56 PASS / 4 FAIL -- the
# exact outcome the guard was written to make impossible.  It now matches the
# line that names a drive, "ata: disk0 = ...", which no diskless boot prints.
#
# A boot that loses the drive is a HOST symptom, not a kernel one: the same
# "no drives detected" appears in logs from campaigns days apart that touched
# nothing near ATA, roughly one boot in eight.  So it is retried once and only
# once, loudly, and only for this one condition -- a diskless boot is never a
# plausible symptom of a kernel change, which is what makes retrying it honest
# rather than a way of grinding until the answer is nice.

[CmdletBinding()]
param(
    [int]    $Boots      = 3,
    [int]    $TimeoutSec = 300,
    [string] $Root       = 'D:\style9\os',
    [switch] $Fresh
)

$ErrorActionPreference = 'Stop'
$disk = Join-Path $Root 'obj\style9.apfs'
$log  = Join-Path $Root 'serial.log'

function Kill-Qemu {
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 50; $i++) {
        if (-not (Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue)) { return }
        Start-Sleep -Milliseconds 200
    }
    throw 'qemu would not die'
}

function Wsl-Path([string] $p) {
    $p = $p.Replace('\', '/')
    return '/mnt/' + $p.Substring(0,1).ToLower() + $p.Substring(2)
}

if ($Fresh) {
    # The image MUST be removed first.  `make disk` is a timestamp rule over
    # the gzipped fixture, so once an image exists it is newer than the fixture
    # for ever and the target is silently up to date -- which means a run asked
    # to start from a pristine volume quietly starts from whatever the last
    # boot left, damage included.  That cost a whole acceptance run once.
    Write-Host '[accept] regenerating obj/style9.apfs from the fixture'
    Remove-Item $disk -ErrorAction SilentlyContinue
    Push-Location $Root
    # Relaxed for the same reason as the apfsck call below: WSL writes a
    # cosmetic notice to stderr on this host, and PowerShell turns a native
    # command's stderr into a TERMINATING error under 'Stop'.
    $ErrorActionPreference = 'Continue'
    wsl make disk 2>&1 | Out-Null
    $ErrorActionPreference = 'Stop'
    Pop-Location
    if (-not (Test-Path $disk)) { throw 'make disk produced no image' }
    $age = (Get-Date) - (Get-Item $disk).LastWriteTime
    if ($age.TotalMinutes -gt 2) { throw 'the image was not rebuilt just now' }
}
if (-not (Test-Path $disk)) { throw "no disk image at $disk" }

# WSL AND QEMU DO NOT SHARE THE VIRTUALISATION STACK ON THIS HOST.  A running
# WSL VM makes the next qemu-system-x86_64 exit before it opens the serial
# port, so the boot produces an EMPTY log -- which the drive check below then
# reports as "the kernel never reported a drive", blaming the kernel for a
# build tool.  It cost three runs before it was recognised.
#
# EVERY boot needs this, not just the first: -Fresh rebuilds the image with
# wsl, and apfsck runs under wsl after each boot, so the VM is back up by the
# time the next one starts.  Shutting it down is instantaneous when it is not
# running.
function Stop-Wsl {
    $ErrorActionPreference = 'Continue'
    wsl --shutdown 2>&1 | Out-Null
    $ErrorActionPreference = 'Stop'
    Start-Sleep -Seconds 2
}

$b = 1
$retried = $false
while ($b -le $Boots) {
    Kill-Qemu
    Stop-Wsl
    Remove-Item $log -ErrorAction SilentlyContinue
    Write-Host "[accept] boot $b/$Boots"
    & (Join-Path $Root 'tools\run.ps1') -Disk $disk | Out-Null

    # WAITING FOR ONE TEST IS NOT WAITING FOR THE BOOT.  This used to stop at
    # `filewrite: done` and kill QEMU, but filewrite is followed by ttyprobe,
    # gstty, the Apple binaries and a shell -- so every boot was cut off at
    # whatever point the three-second poll happened to land, and the PASS
    # tally silently counted a different prefix each time.  One boot in six
    # reported 73 where its neighbours reported 83, with nothing failing.
    #
    # There is no last line to wait for either: a finished boot sits at an
    # interactive prompt.  What says the automated part is over is the log
    # GOING QUIET, so that is what is waited for.
    # AND WAITING FOR THE LOG TO GO QUIET IS NOT WAITING FOR THE BOOT EITHER.
    # The TUI draws a status line with a clock in it, so on a boot that reaches
    # the shell the log never stops growing -- the quiet test then never fires,
    # the boot runs to $TimeoutSec, and the tally is of whatever prefix the
    # clock left behind.  That is the same failure as the one below, wearing a
    # different hat: two boots of one run were counted at 79 and 88 with
    # nothing failing.
    #
    # So the LAST AUTOMATED LINE is what is waited for, with the quiet test
    # kept as the fallback for a boot that dies before reaching it.
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $done = $false
    $lastLen = -1
    $quiet = 0
    $settle = -1
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        if (-not (Test-Path $log)) { continue }
        $len = (Get-Item $log).Length
        if ($len -eq $lastLen) { $quiet++ } else { $quiet = 0; $lastLen = $len }
        $txt = Get-Content $log -Raw -ErrorAction SilentlyContinue
        $ran = $txt -match 'filewrite: (done|\d+ check\(s\) FAILED)'
        # The last thing the boot does on its own: the shell demo finishing.
        if ($txt -match '\[demo\.sh\] done') {
            if ($settle -lt 0) { $settle = 0 } else { $settle++ }
        }
        if ($settle -ge 2) { $done = $true; break }
        if ($ran -and $quiet -ge 4) { $done = $true; break }
    }
    Kill-Qemu
    if (-not $done) { Write-Host "[accept] boot ${b}: TIMED OUT before the boot went quiet" }

    $lines = Get-Content $log -ErrorAction SilentlyContinue
    if (-not ($lines | Select-String -Pattern 'ata: disk0 =' -Quiet)) {
        if (-not $retried) {
            $retried = $true
            Write-Host "[accept] boot ${b}: no drive -- known host flake, retrying once"
            continue
        }
        throw "boot ${b}: the kernel never reported a drive TWICE -- it ran against nothing"
    }
    $retried = $false
    $pass  = ($lines | Select-String -Pattern 'PASS' ).Count
    $fail  = ($lines | Select-String -Pattern 'FAIL' ).Count
    $panic = ($lines | Select-String -Pattern 'panic').Count
    Write-Host "[accept] boot ${b}: $pass PASS / $fail FAIL / $panic panic"
    if ($fail -gt 0) { $lines | Select-String -Pattern 'FAIL' | ForEach-Object { "    $_" } }
    $lines | Select-String -Pattern '^apfs-(room|move):' | ForEach-Object { "    $_" }
    Copy-Item $log (Join-Path $Root "serial-accept$b.log") -Force

    # WSL writes a cosmetic notice to stderr on this host, and PowerShell turns
    # a native command's stderr into a terminating error under 'Stop'.  The
    # exit code is the thing being asked for, so the preference is relaxed
    # around the call rather than the notice being chased.
    # STDERR IS WHERE apfsck SAYS WHAT IS WRONG.  This used to send it to
    # $null, so a failing boot printed "apfsck exit 1:" and then nothing at
    # all -- the instrument going quiet at the exact moment it had something
    # to say.  Cost a boot's worth of guessing.
    $ErrorActionPreference = 'Continue'
    $ck = wsl /usr/sbin/apfsck -c (Wsl-Path $disk) 2>&1
    $rc = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($rc -eq 0) { Write-Host "[accept] boot ${b}: apfsck exit 0" }
    else { Write-Host "[accept] boot ${b}: apfsck exit ${rc}:"; $ck | ForEach-Object { "    $_" } }
    $b++
}
Write-Host '[accept] done'
