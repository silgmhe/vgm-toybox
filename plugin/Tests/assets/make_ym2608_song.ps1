# Generates ym2608.vgm next to this script: two seconds of a YM2608 whose *only* sound is the SSG.
#
# Why it exists: the SSG is a linked device behind the OPN chips, and a mute mask written for the
# parent's channel numbers lands on the SSG's own three channels - so an FM track used to carry an
# SSG voice (see docs/memo/2026-10-25-linked-devices-and-channel-numbering.md). A file whose FM half
# is silent, and whose SSG is loud, turns that into a number: with correct isolation every FM track
# renders nothing, and before the fix tracks 0-2 each rendered one SSG channel.
#
#   .\make_ym2608_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe ym2608.vgm 2 44100 --tracks

$ErrorActionPreference = 'Stop'

$bytes = New-Object System.Collections.Generic.List[byte]

function Add-U32([uint32]$v) {
	$bytes.Add([byte]($v -band 0xFF))
	$bytes.Add([byte](($v -shr 8) -band 0xFF))
	$bytes.Add([byte](($v -shr 16) -band 0xFF))
	$bytes.Add([byte](($v -shr 24) -band 0xFF))
}

function Add-Wait([uint16]$samples) {
	$bytes.Add(0x61)
	$bytes.Add([byte]($samples -band 0xFF))
	$bytes.Add([byte](($samples -shr 8) -band 0xFF))
}

# A 0x61 wait carries 16 bits of samples; a two-second gap needs more than one.
function Add-WaitLong([int]$samples) {
	while ($samples -gt 65535) {
		Add-Wait 65535
		$samples = $samples - 65535
	}
	if ($samples -gt 0) { Add-Wait ([uint16]$samples) }
}

# YM2608 port 0 (0x56) carries the SSG registers 0x00..0x0F; port 1 (0x57) the FM half.
function Add-Ssg([byte]$addr, [byte]$value) {
	$bytes.Add(0x56)
	$bytes.Add($addr)
	$bytes.Add($value)
}

$rate = 44100
$clock = 8000000
$totalSamples = $rate * 2

# --- header, VGM 1.71, 0xF8 bytes, YM2608 clock at 0x48 -------------------------------------------
$bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('Vgm '))   # 0x00
Add-U32 0                     # 0x04 EOF offset, patched at the end
Add-U32 0x171                 # 0x08 version
for ($i = 0; $i -lt 0x0C; $i++) { $bytes.Add(0) }                 # 0x0C SN76489 / YM2413 / GD3 offset
Add-U32 $totalSamples         # 0x18 total samples
Add-U32 0                     # 0x1C loop offset (0 = no loop)
Add-U32 0                     # 0x20 loop samples
Add-U32 $rate                 # 0x24 rate
for ($i = 0; $i -lt 0x0C; $i++) { $bytes.Add(0) }                 # 0x28 flags, YM2612 / YM2151 clocks
Add-U32 0xC4                  # 0x34 data offset, relative to 0x34 -> data starts at 0xF8
for ($i = 0; $i -lt 0x10; $i++) { $bytes.Add(0) }                 # 0x38 SegaPCM / SPCM / RF5C68 / YM2203
Add-U32 $clock                # 0x48 YM2608 clock
for ($i = 0; $i -lt 0x6C; $i++) { $bytes.Add(0) }                 # 0x4C .. 0xB7 the remaining clocks
Add-U32 0                     # 0xB8 (unused by this file)
for ($i = 0; $i -lt 0x3C; $i++) { $bytes.Add(0) }                 # 0xBC .. data offset

if ($bytes.Count -ne 0xF8) { throw ("header is {0} bytes, expected 0xF8" -f $bytes.Count) }

# --- the three SSG voices ------------------------------------------------------------------------
# period = clock / (16 * frequency): A4 440 Hz = 1136 = 0x0470, C5 523 Hz = 956 = 0x03BC,
# E5 659 Hz = 759 = 0x02F7. Low byte first, then the high nibble.
Add-Ssg 0x00 0x70; Add-Ssg 0x01 0x04      # channel A
Add-Ssg 0x02 0xBC; Add-Ssg 0x03 0x03      # channel B
Add-Ssg 0x04 0xF7; Add-Ssg 0x05 0x02      # channel C

# 0x07: the mixer. Bits 0-2 enable the three tone generators, bits 3-5 the noise ones; 0x38 is
# "three tones, no noise", which keeps one voice per channel.
Add-Ssg 0x07 0x38

# 0x08..0x0A: the amplitudes, 0x0F being full scale.
Add-Ssg 0x08 0x0F
Add-Ssg 0x09 0x0F
Add-Ssg 0x0A 0x0F

Add-WaitLong ($totalSamples - 1600)
Add-Ssg 0x08 0x00                         # channel A off, so the last block is not all three
Add-WaitLong 1600

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'ym2608.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
