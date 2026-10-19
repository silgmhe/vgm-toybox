# Generates opl4.vgm next to this script: two seconds of YMF278B (OPL4) FM, one note on each of the
# two OPL3 banks, so the end-to-end check (vgm_mixer_test) has a real file for the 0xD0 command path.
#
#   .\make_opl4_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe opl4.vgm 2 44100 --notes
#
# Expect: A4 (69) on YMF278B channel 0 from 0.00 s to 0.50 s and A4 on channel 9 (the second bank,
# reached through port 1) from 1.00 s to 1.50 s. Both keys sit on block 4, FNUM 0x246:
# 582 * (33868800 * 8 / 19) / 288 / 2^16 = 439.7 Hz, which rounds to A4. The FM clock is the header
# clock times 8/19 because the OPL4 drives its OPL3 at that rate (libvgm's init_opl3_devinfo).
# The header layout follows the VGM offsets libvgm reads (libs/libvgm/player/vgmplayer.cpp,
# _CHIPCLK_OFS): YMF278B clock at 0x60, data offset at 0x34.

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
Add-U32 0                     # 0x0C SN76489 clock
for ($i = 0; $i -lt 0x08; $i++) { $bytes.Add(0) }                 # 0x10 YM2413, Game Gear PSG
Add-U32 $totalSamples         # 0x18 total samples
Add-U32 0                     # 0x1C loop offset (0 = no loop)
Add-U32 0                     # 0x20 loop samples
Add-U32 $rate                 # 0x24 rate
for ($i = 0; $i -lt 0x08; $i++) { $bytes.Add(0) }                 # 0x28 SN76489 flags, YM2612
Add-U32 0                     # 0x30 YM2151 clock
Add-U32 0xC4                  # 0x34 data offset, relative to 0x34 -> data starts at 0xF8
for ($i = 0; $i -lt 0x28; $i++) { $bytes.Add(0) }                 # 0x38 Sega PCM .. 0x5F YMF271
Add-U32 33868800              # 0x60 YMF278B clock
for ($i = 0; $i -lt 0x94; $i++) { $bytes.Add(0) }                 # 0x64 .. 0xF7 unused clocks

if ($bytes.Count -ne 0xF8) { throw ("header is {0} bytes, expected 0xF8" -f $bytes.Count) }

# --- data -----------------------------------------------------------------------------------------
function Add-Opl4([byte]$portByte, [byte]$reg, [byte]$value) {
	$bytes.Add(0xD0)
	$bytes.Add($portByte)
	$bytes.Add($reg)
	$bytes.Add($value)
}

function Add-Wait([int]$samples) {
	$bytes.Add(0x61)
	$bytes.Add([byte]($samples -band 0xFF))
	$bytes.Add([byte](($samples -shr 8) -band 0xFF))
}

# one 2-operator patch on bank 0 channel 0: both operators full volume, fastest attack, op2 carrier
Add-Opl4 0x00 0x20 0x21           # op1: multiplier 1
Add-Opl4 0x00 0x23 0x21           # op2: multiplier 1
Add-Opl4 0x00 0x40 0x00           # op1: no attenuation
Add-Opl4 0x00 0x43 0x00           # op2: no attenuation
Add-Opl4 0x00 0x60 0xF0           # op1: instant attack, fast decay
Add-Opl4 0x00 0x63 0xF0           # op2
Add-Opl4 0x00 0x80 0x00           # op1: no sustain, no release
Add-Opl4 0x00 0x83 0x00           # op2
Add-Opl4 0x00 0xC0 0x00           # FM connection, no feedback
Add-Opl4 0x00 0xA0 0x46           # FNUM 0x246, low byte
Add-Opl4 0x00 0xB0 0x32           # key on, block 4, FNUM high 0x02

Add-Wait 22050

Add-Opl4 0x00 0xB0 0x12           # key off

Add-Wait 22050

# the second bank is a separate port, not a separate register range
Add-Opl4 0x01 0x20 0x21
Add-Opl4 0x01 0x23 0x21
Add-Opl4 0x01 0x40 0x00
Add-Opl4 0x01 0x43 0x00
Add-Opl4 0x01 0x60 0xF0
Add-Opl4 0x01 0x63 0xF0
Add-Opl4 0x01 0x80 0x00
Add-Opl4 0x01 0x83 0x00
Add-Opl4 0x01 0xC0 0x00
Add-Opl4 0x01 0xA0 0x46
Add-Opl4 0x01 0xB0 0x32

Add-Wait 22050

Add-Opl4 0x01 0xB0 0x12           # key off

Add-Wait 22050

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'opl4.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
