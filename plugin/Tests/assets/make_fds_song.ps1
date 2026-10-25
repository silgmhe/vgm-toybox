# Generates fds_song.nsf next to this script: a two-second FDS tone and nothing else.
#
# Why it exists: the Famicom Disk System's wavetable channel was never instantiated by the player,
# so an FDS song lost its only expansion voice to silence - audible in neither the plain nor the
# mixer path (see docs/review/2026-10-25-nsf-playback-experience.md). This asset is the smallest
# file that proves the wiring: a hand-assembled INIT that writes the wavetable and a frequency, and
# a PLAY that does nothing, which is all an FDS needs to keep sounding.
#
# Header offsets follow this repository's reader, formats/nsf/NsfFile.h.
#
#   .\make_fds_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe fds_song.nsf 1 44100 --tracks
#
# Expect six tracks: the five 2A03 ones, all silent, and "FDS" with a non-zero RMS.

$ErrorActionPreference = 'Stop'

$rom = New-Object System.Collections.Generic.List[byte]

function Emit([byte[]]$bytes) { $rom.AddRange($bytes) }

# --- INIT at $8000 ------------------------------------------------------------------------------
Emit @(0xA9, 0x02, 0x8D, 0x23, 0x40)     # LDA #$02 : STA $4023  - FDS master I/O on, sound on
Emit @(0xA9, 0x80, 0x8D, 0x89, 0x40)     # LDA #$80 : STA $4089  - wavetable writes enabled

# The 64-byte wavetable, one STA per byte at $4040 + i. A ramp is the simplest shape that is not flat.
for ($i = 0; $i -lt 64; $i++) {
	$value = [byte](($i * 4) -band 0x3F)
	Emit @(0xA9, $value, 0x8D, [byte](0x40 + $i), 0x40)
}

Emit @(0xA9, 0x00, 0x8D, 0x89, 0x40)     # LDA #$00 : STA $4089  - wavetable writes off
Emit @(0xA9, 0x80, 0x8D, 0x84, 0x40)     # LDA #$80 : STA $4084  - mod envelope off (no modulation)
Emit @(0xA9, 0x80, 0x8D, 0x87, 0x40)     # LDA #$80 : STA $4087  - mod halt
Emit @(0xA9, 0xBF, 0x8D, 0x80, 0x40)     # LDA #$BF : STA $4080  - volume envelope off, level 3F
Emit @(0xA9, 0x00, 0x8D, 0x82, 0x40)     # LDA #$00 : STA $4082  - frequency low
Emit @(0xA9, 0x03, 0x8D, 0x83, 0x40)     # LDA #$03 : STA $4083  - frequency high -> 0x300
Emit @(0xA9, 0x03, 0x8D, 0x8A, 0x40)     # LDA #$03 : STA $408A  - master volume
Emit @(0x60)                             # RTS

$playAddress = 0x8000 + $rom.Count
Emit @(0x60)                             # PLAY: RTS - the FDS sounds on its own

# --- the NSF 1 header ---------------------------------------------------------------------------
$header = New-Object System.Collections.Generic.List[byte]
$header.AddRange([System.Text.Encoding]::ASCII.GetBytes('NESM'))
$header.Add(0x1A)                        # 0x04 signature
$header.Add(1)                           # 0x05 version
$header.Add(1)                           # 0x06 total songs
$header.Add(1)                           # 0x07 starting song

function Add-U16([uint16]$v) {
	$header.Add([byte]($v -band 0xFF))
	$header.Add([byte](($v -shr 8) -band 0xFF))
}

Add-U16 0x8000                           # 0x08 load address
Add-U16 0x8000                           # 0x0A init address
Add-U16 ([uint16]$playAddress)           # 0x0C play address

$name = [System.Text.Encoding]::ASCII.GetBytes('FDS tone')
$header.AddRange($name)

while ($header.Count -lt 0x6E) { $header.Add(0) }
Add-U16 16666                            # 0x6E NTSC speed, microseconds per frame
while ($header.Count -lt 0x78) { $header.Add(0) }
Add-U16 19997                            # 0x78 PAL speed
$header.Add(0x04)                        # 0x7A chip flags: bit 2 declares the FDS
while ($header.Count -lt 0x80) { $header.Add(0) }

$bytes = New-Object System.Collections.Generic.List[byte]
$bytes.AddRange($header)
$bytes.AddRange($rom)

$out = Join-Path $PSScriptRoot 'fds_song.nsf'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, header {2} + rom {3}, play at 0x{4:X4}, flags 0x{5:X2})" -f $out, $bytes.Count, $header.Count, $rom.Count, $playAddress, 0x04
