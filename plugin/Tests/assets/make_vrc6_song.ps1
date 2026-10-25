# Generates vrc6_song.nsf next to this script: two seconds of a VRC6 pulse and its ramp.
#
# Why it exists: the VRC6 is the one expansion chip whose channels the keyboard could already draw
# before the others existed, but the corpus has no VRC6 file - every NSF in test_file/nsf declares
# the N163 or nothing. Without one, the table that describes VRC6 channels was never exercised, and
# a wrong channel order would have gone out looking right. Two voices, not one, because the point
# is the order: the track list puts the ramp first while the chip numbers it last.
#
# Registers (nes_vrc6.cpp, case 0x9000..):
#   $9000  pulse 1: volume 0-15, bits 4-6 duty, bit 7 gate (1 = off)
#   $9001  pulse 1 period low
#   $9002  pulse 1 period high (bits 0-3), bit 7 enable
#   $B000  ramp volume 0-63
#   $B001  ramp period low
#   $B002  ramp period high (bits 0-3), bit 7 enable
#
#   .\make_vrc6_song.ps1
#   ..\..\..\build\plugin\vgm_nsf_probe.exe vrc6_song.nsf --chips
#
# Expect eight tracks - five 2A03 and Saw Wave / Square 3 / Square 4 - of which the ramp carries
# 220 Hz and Square 3 carries 440 Hz.

$ErrorActionPreference = 'Stop'

$rom = New-Object System.Collections.Generic.List[byte]

function Emit([byte[]]$bytes) { $rom.AddRange($bytes) }

# --- INIT at $8000 ------------------------------------------------------------------------------
# Pulse 1: volume 15, duty 0, gate open, then 0x0FD -> 1789772/16/254 = 440 Hz.
Emit @(0xA9, 0x0F, 0x8D, 0x00, 0x90)     # LDA #$0F : STA $9000 - volume 15, duty 0, gate open
Emit @(0xA9, 0xFD, 0x8D, 0x01, 0x90)     # LDA #$FD : STA $9001 - period low
Emit @(0xA9, 0x80, 0x8D, 0x02, 0x90)     # LDA #$80 : STA $9002 - period high 0, enable

# Ramp: volume 63, then 0x244 -> 1789772/14/581 = 220 Hz.
Emit @(0xA9, 0x3F, 0x8D, 0x00, 0xB0)     # LDA #$3F : STA $B000 - ramp volume 63
Emit @(0xA9, 0x44, 0x8D, 0x01, 0xB0)     # LDA #$44 : STA $B001 - period low
Emit @(0xA9, 0x82, 0x8D, 0x02, 0xB0)     # LDA #$82 : STA $B002 - period high 2, enable
Emit @(0x60)                             # RTS

$playAddress = 0x8000 + $rom.Count
Emit @(0x60)                             # PLAY: RTS - both channels sustain on their own

$header = New-Object System.Collections.Generic.List[byte]
$header.AddRange([System.Text.Encoding]::ASCII.GetBytes('NESM'))
$header.Add(0x1A)
$header.Add(1)                           # version
$header.Add(1)                           # songs
$header.Add(1)                           # starting song

function Add-U16([uint16]$v) {
	$header.Add([byte]($v -band 0xFF))
	$header.Add([byte](($v -shr 8) -band 0xFF))
}

Add-U16 0x8000                           # load address
Add-U16 0x8000                           # init address
Add-U16 ([uint16]$playAddress)           # play address

$header.AddRange([System.Text.Encoding]::ASCII.GetBytes('VRC6 tone'))
while ($header.Count -lt 0x6E) { $header.Add(0) }
Add-U16 16666                            # NTSC speed
while ($header.Count -lt 0x78) { $header.Add(0) }
Add-U16 19997                            # PAL speed
$header.Add(0x01)                        # chip flags: bit 0 declares the VRC6
while ($header.Count -lt 0x80) { $header.Add(0) }

$bytes = New-Object System.Collections.Generic.List[byte]
$bytes.AddRange($header)
$bytes.AddRange($rom)

$out = Join-Path $PSScriptRoot 'vrc6_song.nsf'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, play at 0x{2:X4})" -f $out, $bytes.Count, $playAddress
