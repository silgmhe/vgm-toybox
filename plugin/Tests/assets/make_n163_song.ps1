# Generates n163_song.nsf next to this script: one N163 wave channel, sounding.
#
# Why it exists: the backend never declared the N163 to the mixer, so a file that uses one showed
# a chip card with the 2A03 alone while the mixer drew all eight of its channels underneath. No
# file in the corpus declares the chip - the one that looked like it (Entry080) turned out to be a
# VRC6 once the header was read properly - so nothing in the suite could catch it.
#
# Registers (nes_n106.cpp): the chip has no address space of its own, only two ports.
#   $F800  register number; bit 7 on means "advance after each write"
#   $4800  the value
#   $E000  master switch; left alone, which is enabled, and reg 0x7F stays 0 = one channel
#
# Then, per channel, eight bytes from 0x40 + channel*8:
#   +0/+2 frequency low/mid, +4 frequency high 2 bits and the length ($44 & 0xFC is 256 - len)
#   +6 wave offset, +7 volume (low nibble)
# The wave RAM is regs 0x00..0x7F, two four-bit samples per byte; one channel means all 128 bytes
# belong to it. Only the first eight are written - the rest stay zero, which is the second half
# of a square.
#
#   .\make_n163_song.ps1
#   ..\..\..\build\plugin\vgm_nsf_probe.exe n163_song.nsf --chips
#
# Expect thirteen channels, 2A03 and N163, with Wave 8 sounding at 440 Hz. That is the row trk 0
# answers with: the chip numbers its channels backwards, so the one channel a one-channel file
# gets is the last one.

$ErrorActionPreference = 'Stop'

$rom = New-Object System.Collections.Generic.List[byte]

function Emit([byte[]]$bytes) { $rom.AddRange($bytes) }

# --- INIT at $8000 ------------------------------------------------------------------------------
# Wave RAM, first eight bytes: two samples of 15 each. Auto-advance because this is a run, and
# eight STA $4800 in a row because A does not change.
Emit @(0xA9, 0x80, 0x8D, 0x00, 0xF8)     # LDA #$80 : STA $F800 - register 0, advance
Emit @(0xA9, 0xFF)                       # LDA #$FF - two samples of 15
Emit @(0x8D, 0x00, 0x48)                 # STA $4800 (0)
Emit @(0x8D, 0x00, 0x48)                 # STA $4800 (1)
Emit @(0x8D, 0x00, 0x48)                 # STA $4800 (2)
Emit @(0x8D, 0x00, 0x48)                 # STA $4800 (3)
Emit @(0x8D, 0x00, 0x48)                 # STA $4800 (4)
Emit @(0x8D, 0x00, 0x48)                 # STA $4800 (5)
Emit @(0x8D, 0x00, 0x48)                 # STA $4800 (6)
Emit @(0x8D, 0x00, 0x48)                 # STA $4800 (7)

# The channel a one-channel chip uses is channel 7. GetTrackInfo indexes with 7 - trk, so trk 0
# - the first row - is the last channel; with one channel enabled that is the only row with
# anything to read, and its registers start at 0x78. 0x1E36 over a 32-sample wave gives
# 7734 * 1789772 / (15 * 65536 * 1 * 32) = 440 Hz.
Emit @(0xA9, 0x78, 0x8D, 0x00, 0xF8)     # LDA #$78 : STA $F800 - frequency low
Emit @(0xA9, 0x36, 0x8D, 0x00, 0x48)     # LDA #$36 : STA $4800
Emit @(0xA9, 0x7A, 0x8D, 0x00, 0xF8)     # LDA #$7A : STA $F800 - frequency mid
Emit @(0xA9, 0x1E, 0x8D, 0x00, 0x48)     # LDA #$1E : STA $4800
Emit @(0xA9, 0x7C, 0x8D, 0x00, 0xF8)     # LDA #$7C : STA $F800 - high bits and length
Emit @(0xA9, 0xE0, 0x8D, 0x00, 0x48)     # LDA #$E0 : STA $4800 - 256 - 0xE0 = 32 samples
Emit @(0xA9, 0x7E, 0x8D, 0x00, 0xF8)     # LDA #$7E : STA $F800 - wave offset
Emit @(0xA9, 0x00, 0x8D, 0x00, 0x48)     # LDA #$00 : STA $4800
Emit @(0xA9, 0x7F, 0x8D, 0x00, 0xF8)     # LDA #$7F : STA $F800 - count and volume
Emit @(0xA9, 0x0F, 0x8D, 0x00, 0x48)     # LDA #$0F : STA $4800 - high nibble 0 = one channel, low 15 = volume
Emit @(0x60)                             # RTS

$playAddress = 0x8000 + $rom.Count
Emit @(0x60)                             # PLAY: RTS - the channel sustains on its own

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

$header.AddRange([System.Text.Encoding]::ASCII.GetBytes('N163 wave'))
while ($header.Count -lt 0x6E) { $header.Add(0) }
Add-U16 16666                            # NTSC speed
while ($header.Count -lt 0x78) { $header.Add(0) }
Add-U16 19997                            # PAL speed
$header.Add(0x00)                        # 0x7A: the format's own chip byte, left empty
$header.Add(0x10)                        # 0x7B: where every file in the wild actually puts it
while ($header.Count -lt 0x80) { $header.Add(0) }

$bytes = New-Object System.Collections.Generic.List[byte]
$bytes.AddRange($header)
$bytes.AddRange($rom)

$out = Join-Path $PSScriptRoot 'n163_song.nsf'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, play at 0x{2:X4})" -f $out, $bytes.Count, $playAddress
