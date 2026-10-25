# Generates fme7_song.nsf next to this script: two seconds of one S5B (FME-7) tone channel.
#
# Why it exists: the S5B was instantiated and given tracks, but a track only means something if the
# chip reaches it. No FME7 NSF was available to test with, so the file is assembled here.
#
# The S5B takes its registers through a pair of addresses: $C000 selects the register number and
# $E000 writes the value. Register 0x00/0x01 are channel A's period, 0x07 is the mixer (a cleared
# bit enables a channel), and 0x08..0x0A are the three amplitudes.
#
#   .\make_fme7_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe fme7_song.nsf 1 44100 --tracks
#
# Expect eight tracks: the five 2A03 ones, all silent, and Square 1..3, of which the first carries
# the tone.

$ErrorActionPreference = 'Stop'

$rom = New-Object System.Collections.Generic.List[byte]

function Emit([byte[]]$bytes) { $rom.AddRange($bytes) }

function Reg([byte]$index, [byte]$value) {
	Emit @(0xA9, $index, 0x8D, 0x00, 0xC0)   # LDA #index : STA $C000 - select
	Emit @(0xA9, $value, 0x8D, 0x00, 0xE0)   # LDA #value : STA $E000 - write
}

# --- INIT at $8000 ------------------------------------------------------------------------------
Reg 0x07 0x38                             # mixer: tones A, B and C on, no noise
Reg 0x00 0x00                             # channel A period low
Reg 0x01 0x01                             # channel A period high -> 0x0100
Reg 0x08 0x0F                             # channel A volume, full
Emit @(0x60)                              # RTS

$playAddress = 0x8000 + $rom.Count
Emit @(0x60)                              # PLAY: RTS - the tone sustains on its own

$header = New-Object System.Collections.Generic.List[byte]
$header.AddRange([System.Text.Encoding]::ASCII.GetBytes('NESM'))
$header.Add(0x1A)
$header.Add(1)
$header.Add(1)
$header.Add(1)

function Add-U16([uint16]$v) {
	$header.Add([byte]($v -band 0xFF))
	$header.Add([byte](($v -shr 8) -band 0xFF))
}

Add-U16 0x8000
Add-U16 0x8000
Add-U16 ([uint16]$playAddress)

$header.AddRange([System.Text.Encoding]::ASCII.GetBytes('FME7 tone'))
while ($header.Count -lt 0x6E) { $header.Add(0) }
Add-U16 16666
while ($header.Count -lt 0x78) { $header.Add(0) }
Add-U16 19997
$header.Add(0x20)                         # chip flags: bit 5 declares the S5B / FME-7
while ($header.Count -lt 0x80) { $header.Add(0) }

$bytes = New-Object System.Collections.Generic.List[byte]
$bytes.AddRange($header)
$bytes.AddRange($rom)

$out = Join-Path $PSScriptRoot 'fme7_song.nsf'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, play at 0x{2:X4})" -f $out, $bytes.Count, $playAddress
