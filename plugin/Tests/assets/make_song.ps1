# Generates minimal.vgm next to this script: six seconds of a 440 Hz SN76489 tone, no loop.
#
# The tests need a song that is *in the repository* - the mixer gate renders one twice, and the
# window scripts load one - but a real VGM cannot be committed (copyright, size). So the song is
# generated: 431 bytes, public domain by construction, and regenerable if the format ever needs it.
#
#   .\make_song.ps1
#
# The layout follows the VGM 1.50 header as libvgm parses it (libs/libvgm/player/vgmplayer.cpp,
# ParseHeader(): data offset at 0x34 read relative to 0x34, total samples at 0x18, no loop).

$ErrorActionPreference = 'Stop'

$bytes = New-Object System.Collections.Generic.List[byte]

function Add-U32([uint32]$v) {
	$bytes.Add([byte]($v -band 0xFF))
	$bytes.Add([byte](($v -shr 8) -band 0xFF))
	$bytes.Add([byte](($v -shr 16) -band 0xFF))
	$bytes.Add([byte](($v -shr 24) -band 0xFF))
}

$rate = 44100
$seconds = 6
$totalSamples = $rate * $seconds

# --- header, VGM 1.50, 0x40 bytes ---------------------------------------------------------------
$bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('Vgm '))   # 0x00
Add-U32 0                     # 0x04 EOF offset, patched at the end
Add-U32 0x150                 # 0x08 version
Add-U32 3579545               # 0x0C SN76489 clock
Add-U32 0                     # 0x10 YM2413 clock
Add-U32 0                     # 0x14 GD3 offset
Add-U32 $totalSamples         # 0x18 total samples
Add-U32 0                     # 0x1C loop offset (0 = no loop)
Add-U32 0                     # 0x20 loop samples
Add-U32 $rate                 # 0x24 rate
for ($i = 0; $i -lt 12; $i++) { $bytes.Add(0) }   # 0x28 SN feedback/shift/flags, YM2612 clock
Add-U32 0x0C                  # 0x34 data offset, relative to 0x34 -> data starts at 0x40
for ($i = 0; $i -lt 8; $i++) { $bytes.Add(0) }    # 0x38 reserved

if ($bytes.Count -ne 0x40) { throw ("header is {0} bytes, expected 0x40" -f $bytes.Count) }

# --- data: one note, then 735-sample waits -------------------------------------------------------
function Add-Write([byte]$v) {
	$bytes.Add(0x50)      # SN76489 write
	$bytes.Add($v)
}

Add-Write 0x8E   # channel 0, tone, low nibble of N
Add-Write 0x0F   # high 6 bits of N -> N = 0x0FE = 254 -> 3579545 / (32 * 254) = 440 Hz
Add-Write 0x90   # channel 0 attenuation 0 (loudest)

$remaining = $totalSamples
while ($remaining -gt 0) {
	$bytes.Add(0x62)      # wait 735 samples
	$remaining -= 735
}

$bytes.Add(0x66) # end of sound data

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'minimal.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
