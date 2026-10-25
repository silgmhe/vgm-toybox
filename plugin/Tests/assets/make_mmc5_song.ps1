# Generates mmc5_song.nsf next to this script: two seconds of one MMC5 pulse channel.
#
# Why it exists: the MMC5's two pulse channels and its PCM voice were never instantiated by the
# player, so a song that declares the chip lost them in silence. There is no way to test the wiring
# without a file, and MMC5 NSFs are rare in the wild - so the file is assembled here, a few bytes of
# 6502 that turn one channel on and return.
#
# Registers (nes_mmc5.cpp, case 0x5000..):
#   $5015  enable: bit 0 pulse 1, bit 1 pulse 2
#   $5000  pulse 1: volume 0-3, bit 4 constant volume, bits 6-7 duty
#   $5002  pulse 1 period low
#   $5003  pulse 1 period high (bits 0-2) and length index (bits 3-7)
#
#   .\make_mmc5_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe mmc5_song.nsf 1 44100 --tracks
#
# Expect eight tracks: the five 2A03 ones, all silent, and Square 1 / Square 2 / PCM, of which the
# first carries the tone.

$ErrorActionPreference = 'Stop'

$rom = New-Object System.Collections.Generic.List[byte]

function Emit([byte[]]$bytes) { $rom.AddRange($bytes) }

# --- INIT at $8000 ------------------------------------------------------------------------------
Emit @(0xA9, 0x03, 0x8D, 0x15, 0x50)     # LDA #$03 : STA $5015 - enable both pulse channels
Emit @(0xA9, 0xBF, 0x8D, 0x00, 0x50)     # LDA #$BF : STA $5000 - duty 2, constant volume 15
Emit @(0xA9, 0x7F, 0x8D, 0x02, 0x50)     # LDA #$7F : STA $5002 - period low
Emit @(0xA9, 0x3A, 0x8D, 0x03, 0x50)     # LDA #$3A : STA $5003 - period high 2, length index 7
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

$header.AddRange([System.Text.Encoding]::ASCII.GetBytes('MMC5 tone'))
while ($header.Count -lt 0x6E) { $header.Add(0) }
Add-U16 16666                            # NTSC speed
while ($header.Count -lt 0x78) { $header.Add(0) }
Add-U16 19997                            # PAL speed
$header.Add(0x08)                        # chip flags: bit 3 declares the MMC5
while ($header.Count -lt 0x80) { $header.Add(0) }

$bytes = New-Object System.Collections.Generic.List[byte]
$bytes.AddRange($header)
$bytes.AddRange($rom)

$out = Join-Path $PSScriptRoot 'mmc5_song.nsf'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, play at 0x{2:X4})" -f $out, $bytes.Count, $playAddress
