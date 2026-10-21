# Generates psg.vgm next to this script: two seconds of SN76489 + AY-3-8910 + RF5C68, one note per
# chip family so the end-to-end check (vgm_mixer_test) has a real file to decode.
#
#   .\make_psg_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe psg.vgm 3 44100 --notes
#
# Expect: AY channel A holds A4 (69) from 0.00 s to 0.50 s, SN76489 channel 0 A4 (69) from 0.50 s to
# 1.50 s, SN76489 noise a drum key at 1.00 s, RF5C68 channel 0 the C4 sample marker at 1.00 s (80 ms).
# The header layout follows the VGM 1.71 offsets libvgm reads (libs/libvgm/player/vgmplayer.cpp,
# ParseHeader(): SN76489 clock at 0x0C, RF5C68 at 0x60, AY8910 at 0x74, data offset at 0x34).

$ErrorActionPreference = 'Stop'

$bytes = New-Object System.Collections.Generic.List[byte]

function Add-U32([uint32]$v) {
	$bytes.Add([byte]($v -band 0xFF))
	$bytes.Add([byte](($v -shr 8) -band 0xFF))
	$bytes.Add([byte](($v -shr 16) -band 0xFF))
	$bytes.Add([byte](($v -shr 24) -band 0xFF))
}

$rate = 44100
$totalSamples = $rate * 2

# --- header ---------------------------------------------------------------------------------------
$bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('Vgm '))   # 0x00
Add-U32 0                     # 0x04 EOF offset, patched at the end
Add-U32 0x171                 # 0x08 version 1.71
Add-U32 3579545               # 0x0C SN76489 clock
for ($i = 0; $i -lt 0x08; $i++) { $bytes.Add(0) }                 # 0x10 YM2413, Game Gear PSG
Add-U32 $totalSamples         # 0x18 total samples
Add-U32 0                     # 0x1C loop offset (0 = no loop)
Add-U32 0                     # 0x20 loop samples
Add-U32 $rate                 # 0x24 rate
for ($i = 0; $i -lt 0x08; $i++) { $bytes.Add(0) }                 # 0x28 SN76489 flags, YM2612
Add-U32 0                     # 0x30 YM2151 clock
Add-U32 0xC4                  # 0x34 data offset, relative to 0x34 -> data starts at 0xF8
for ($i = 0; $i -lt 0x08; $i++) { $bytes.Add(0) }                 # 0x38 Sega PCM
Add-U32 12500000              # 0x40 RF5C68 clock
for ($i = 0; $i -lt 0x30; $i++) { $bytes.Add(0) }                 # 0x44 .. 0x73 unused clocks
Add-U32 1789773               # 0x74 AY8910 clock
for ($i = 0; $i -lt 0x80; $i++) { $bytes.Add(0) }                 # 0x78 .. 0xF7 unused

if ($bytes.Count -ne 0xF8) { throw ("header is {0} bytes, expected 0xF8" -f $bytes.Count) }

# --- data -----------------------------------------------------------------------------------------
function Add-Sn([byte]$value) {
	$bytes.Add(0x50)
	$bytes.Add($value)
}

function Add-Ay([byte]$reg, [byte]$value) {
	$bytes.Add(0xA0)
	$bytes.Add($reg)
	$bytes.Add($value)
}

function Add-Rf([byte]$reg, [byte]$value) {
	$bytes.Add(0xB0)
	$bytes.Add($reg)
	$bytes.Add($value)
}

function Add-Wait([int]$samples) {
	$bytes.Add(0x61)
	$bytes.Add([byte]($samples -band 0xFF))
	$bytes.Add([byte](($samples -shr 8) -band 0xFF))
}

$step = 22050

# AY channel A: 1789773 / (16 * 254) = 440.4 Hz
Add-Ay 0x00 0xFE
Add-Ay 0x01 0x00
Add-Ay 0x07 0x3E
Add-Ay 0x08 0x0F

Add-Wait $step

# AY off, SN76489 channel 0 on: 3579545 / (32 * 254) = 440.4 Hz
Add-Ay 0x07 0x3F
Add-Sn 0x8E                   # latch: channel 0, tone, low nibble
Add-Sn 0x0F                   # high six bits -> divider 0x0FE
Add-Sn 0x90                   # channel 0 volume, attenuation 0

Add-Wait $step

# SN76489 noise (mode 4 = white, rate 0) plus RF5C68 channel 0
Add-Sn 0xE4
Add-Sn 0xF0                   # channel 3 volume
Add-Rf 0x00 0xFF              # envelope: a zero here is silence, as on the chip
Add-Rf 0x08 0xFE              # channel 0 enabled

Add-Wait $step

# SN76489 channel 0 off
Add-Sn 0x9F

Add-Wait $step

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'psg.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
