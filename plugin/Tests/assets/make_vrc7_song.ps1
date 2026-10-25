# Generates vrc7_song.nsf next to this script: two seconds of one VRC7 FM voice.
#
# Why it exists: the VRC7's six voices had no describe path at all, so a VRC7 song showed a chip name
# and no rows. Nothing in test_file/nsf declares the chip, and it is the one expansion whose channels
# have no waveform to read - the panel has to synthesise one - so the shape kind needs an assertion
# of its own. One voice is enough: what is being checked is that the rows exist, that the sounding
# one says so, and that it carries a pitch.
#
# Registers (nes_vrc7.cpp): the chip exposes exactly two addresses, both of them the OPLL's ports.
#   $9010  register number
#   $9030  data
# Then (emu2413): 0x30+ch instrument/volume, 0x10+ch fnum low, 0x20+ch key on / block / fnum high.
#
#   .\make_vrc7_song.ps1
#   ..\..\..\build\plugin\vgm_nsf_probe.exe vrc7_song.nsf --chips
#
# Expect eleven tracks - five 2A03, six FM - of which FM 1 is active at 437 Hz.

$ErrorActionPreference = 'Stop'

$rom = New-Object System.Collections.Generic.List[byte]

function Emit([byte[]]$bytes) { $rom.AddRange($bytes) }

# --- INIT at $8000 ------------------------------------------------------------------------------
# Volume first, so the key-on that follows is not a note at zero level. Instrument 1, not 0: 0 is
# the custom slot, and this file never loads a patch into it, so a voice on 0 is silent.
Emit @(0xA9, 0x30, 0x8D, 0x10, 0x90)     # LDA #$30 : STA $9010 - register 0x30
Emit @(0xA9, 0x10, 0x8D, 0x30, 0x90)     # LDA #$10 : STA $9030 - instrument 1, volume 0 (loudest)

Emit @(0xA9, 0x10, 0x8D, 0x10, 0x90)     # LDA #$10 : STA $9010 - register 0x10
Emit @(0xA9, 0x48, 0x8D, 0x30, 0x90)     # LDA #$48 : STA $9030 - fnum low: 72

# Block 6, fnum high bit 0, key on. The chip halves the clock it was given by 36 before using
# it, so the pitch is (1789772/36) * 72 / 2^(19-6) = 437 Hz, near A4.
Emit @(0xA9, 0x20, 0x8D, 0x10, 0x90)     # LDA #$20 : STA $9010 - register 0x20
Emit @(0xA9, 0x1C, 0x8D, 0x30, 0x90)     # LDA #$1C : STA $9030 - key on, block 6
Emit @(0x60)                             # RTS

$playAddress = 0x8000 + $rom.Count
Emit @(0x60)                             # PLAY: RTS - the voice sustains on its own

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

$header.AddRange([System.Text.Encoding]::ASCII.GetBytes('VRC7 tone'))
while ($header.Count -lt 0x6E) { $header.Add(0) }
Add-U16 16666                            # NTSC speed
while ($header.Count -lt 0x78) { $header.Add(0) }
Add-U16 19997                            # PAL speed
$header.Add(0x02)                        # chip flags: bit 1 declares the VRC7
while ($header.Count -lt 0x80) { $header.Add(0) }

$bytes = New-Object System.Collections.Generic.List[byte]
$bytes.AddRange($header)
$bytes.AddRange($rom)

$out = Join-Path $PSScriptRoot 'vrc7_song.nsf'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, play at 0x{2:X4})" -f $out, $bytes.Count, $playAddress
