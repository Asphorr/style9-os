# Type into a running style9-os the way a person would.
#
# QEMU's monitor `sendkey` injects a scancode at the emulated PS/2 controller,
# so a key sent here travels the entire real road: i8042 -> IRQ1 -> the ring in
# dev/kbd.c -> the kbd-drv thread -> whichever consumer holds the console.
# That is the difference between testing the keyboard and testing a function
# that pretends to be one, and it is why the interactive shell is checked this
# way rather than by calling darwin_cons_input from a demo.
#
# Requires the VM to have been started with:  .\tools\run.ps1 -MonitorPort 4444
#
# Usage:
#   .\tools\sendkeys.ps1 -Text "echo hello"      # types it, no Enter
#   .\tools\sendkeys.ps1 -Text "ls" -Enter       # types it and presses Enter
#   .\tools\sendkeys.ps1 -Keys ctrl-d            # raw sendkey names
#   .\tools\sendkeys.ps1 -Text "sleep" -Keys ctrl-c
#
# -DelayMs spaces the keystrokes out.  The default is deliberately non-zero:
# a burst faster than the guest drains its ring proves nothing about a
# terminal, and a dropped key looks exactly like a bug in the code under test.

[CmdletBinding()]
param(
    [string] $Text     = '',
    [string[]] $Keys   = @(),
    [switch] $Enter,
    [int]    $Port     = 4444,
    [int]    $DelayMs  = 30,
    [int]    $SettleMs = 400
)

# US-layout key names for the printable ASCII this kernel can render.  A
# character needing Shift is sent as shift-<key>; QEMU has no "type this
# string" primitive, so the mapping has to live somewhere and this is it.
$plain = @{
    ' ' = 'spc';  '-' = 'minus'; '=' = 'equal'; '[' = 'bracket_left'
    ']' = 'bracket_right'; ';' = 'semicolon'; "'" = 'apostrophe'
    '`' = 'grave_accent'; '\' = 'backslash'; ',' = 'comma'; '.' = 'dot'
    '/' = 'slash'
}
$shifted = @{
    '!' = '1'; '@' = '2'; '#' = '3'; '$' = '4'; '%' = '5'; '^' = '6'
    '&' = '7'; '*' = '8'; '(' = '9'; ')' = '0'; '_' = 'minus'
    '+' = 'equal'; '{' = 'bracket_left'; '}' = 'bracket_right'
    ':' = 'semicolon'; '"' = 'apostrophe'; '~' = 'grave_accent'
    '|' = 'backslash'; '<' = 'comma'; '>' = 'dot'; '?' = 'slash'
}

function Convert-CharToKey([char] $c) {
    if ($c -cmatch '[a-z0-9]') { return "$c" }
    if ($c -cmatch '[A-Z]')    { return "shift-$([char]::ToLower($c))" }
    if ($plain.ContainsKey("$c"))   { return $plain["$c"] }
    if ($shifted.ContainsKey("$c")) { return "shift-$($shifted["$c"])" }
    throw "sendkeys: no key name for '$c'"
}

$keyList = @()
foreach ($c in $Text.ToCharArray()) { $keyList += (Convert-CharToKey $c) }
$keyList += $Keys
if ($Enter) { $keyList += 'ret' }
if ($keyList.Count -eq 0) { Write-Error 'nothing to send'; exit 1 }

$client = New-Object System.Net.Sockets.TcpClient
try {
    $client.Connect('127.0.0.1', $Port)
} catch {
    Write-Error "cannot reach the QEMU monitor on port $Port -- was the VM started with -MonitorPort $Port ?"
    exit 1
}
$stream = $client.GetStream()
$writer = New-Object System.IO.StreamWriter($stream)
$writer.AutoFlush = $true

# The monitor greets us; let it finish before typing over the banner.
Start-Sleep -Milliseconds 200

foreach ($k in $keyList) {
    $writer.WriteLine("sendkey $k")
    Start-Sleep -Milliseconds $DelayMs
}

# Give the guest time to act on the last key before the caller reads the log.
Start-Sleep -Milliseconds $SettleMs
$writer.Close()
$client.Close()
Write-Host "sent $($keyList.Count) key(s)"
