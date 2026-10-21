# Generates ga20.vgm next to this script: two GA20 voices, four notes each - one melodic, one
# percussion - played out of a ROM block.
#
# Why it exists: the GA20 (M4 in docs/desgin/midi-output-design.md) is the fourth sampled player and
# the simplest, which is exactly why its two quirks are easy to get wrong:
#
#   the address unit     the start registers are not a byte address: offset 0 supplies bits 4..11
#                        and offset 1 bits 12..19, so a sample starts at ((reg1 << 8) | reg0) << 4.
#                        This asset starts at 0x4000, which is register value 0x0400.
#   the end              there is no usable end register - a sample ends where its ROM says 0x00
#                        (iremga20_cache_samples calls it the sample end marker). The tone here is
#                        512 bytes followed by the block's zeroes, so the note's length is the
#                        sample's length, not a guessed gate.
#   the rate             the rate register is a divider: one byte every 256-rate output samples, so
#                        the byte rate is 44100/(256-rate) and rate 249 plays 6300 bytes a second.
#                        A 16-byte cycle is therefore 393.75 Hz - G4.
#
# Voice 0 plays the periodic waveform and must come out as G4. Voice 1 plays noise, which has no
# period, so it must come out as the C4 drum trigger - the same convention the other sampled players
# use, because these boards put drums on the same four voices as the music.
#
# The ROM block type is 0x93 - the GA20's entry in libvgm's ROM table (vgmplayer_cmdhandler.cpp,
# _VGM_ROM_CHIPS), and the command is 0xBF (`BF aa dd`, register aa, value dd).
#
#   .\make_ga20_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe ga20.vgm 2 44100 --notes

$ErrorActionPreference = 'Stop'

$bytes = New-Object System.Collections.Generic.List[byte]

function Add-U32([uint32]$v) {
	$bytes.Add([byte]($v -band 0xFF))
	$bytes.Add([byte](($v -shr 8) -band 0xFF))
	$bytes.Add([byte](($v -shr 16) -band 0xFF))
	$bytes.Add([byte](($v -shr 24) -band 0xFF))
}

function Add-U16([uint16]$v) {
	$bytes.Add([byte]($v -band 0xFF))
	$bytes.Add([byte](($v -shr 8) -band 0xFF))
}

function Add-Wait([uint16]$samples) {
	$bytes.Add(0x61)
	$bytes.Add([byte]($samples -band 0xFF))
	$bytes.Add([byte](($samples -shr 8) -band 0xFF))
}

# GA20 register write: 0xBF <register> <value>, register = channel * 8 + n.
function Add-Ga20([byte]$register, [byte]$value) {
	$bytes.Add(0xBF)
	$bytes.Add($register)
	$bytes.Add($value)
}

# One voice: start (two 16-byte-unit halves), rate, volume - then the control bit that keys it on.
function Add-Ga20Voice([int]$channel, [uint16]$start, [byte]$rate, [byte]$volume) {
	$base = [byte]($channel * 8)
	Add-Ga20 ([byte]($base + 0)) ([byte](($start -shr 4) -band 0xFF))
	Add-Ga20 ([byte]($base + 1)) ([byte](($start -shr 12) -band 0xFF))
	Add-Ga20 ([byte]($base + 4)) $rate
	Add-Ga20 ([byte]($base + 5)) $volume
}

$rate = 44100
$noteSamples = [int]($rate / 10)
$notes = 4
$totalSamples = [uint32]($noteSamples * 2 * $notes)

$speed = 249									# 44100 / (256-249) = 6300 bytes a second
$cycle = 16										# 6300 / 16 = 393.75 Hz, G4
$length = 512
$start0 = 0x4000
$start1 = 0x5000
$romSize = 0x10000

# --- header ---------------------------------------------------------------------------------------
$bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('Vgm '))   # 0x00
Add-U32 0                     # 0x04 EOF offset, patched at the end
Add-U32 0x171                 # 0x08 version 1.71 (the version that carries the GA20 clock)
Add-U32 0                     # 0x0C SN76489 clock
Add-U32 0                     # 0x10 YM2413 clock
Add-U32 0                     # 0x14 GD3 offset
Add-U32 $totalSamples         # 0x18 total samples
Add-U32 0                     # 0x1C loop offset
Add-U32 0                     # 0x20 loop samples
Add-U32 $rate                 # 0x24 rate
for ($i = 0; $i -lt 0x04; $i++) { $bytes.Add(0) }                 # 0x28 SN76489 feedback/shift/flags
Add-U32 0                     # 0x2C YM2612 clock
Add-U32 0                     # 0x30 YM2151 clock
Add-U32 0xCC                  # 0x34 data offset, relative to 0x34 -> data starts at 0x100
Add-U32 0                     # 0x38 Sega PCM clock
Add-U32 0                     # 0x3C Sega PCM interface, unused
while ($bytes.Count -lt 0x94) { Add-U32 0 }                       # 0x40 .. 0x93: unused clocks
Add-U16 0                     # 0x94 K051649 clock, unused
$bytes.Add(0x00)              # 0x96 C140 banking type
$bytes.Add(0x00)              # 0x97
while ($bytes.Count -lt 0xB4) { $bytes.Add(0) }                    # 0x98 .. 0xB3: MSM6295/C140/K053260
Add-U32 0                     # 0xB4 QSound clock, unused
while ($bytes.Count -lt 0xE0) { Add-U32 0 }                       # 0xB8 .. 0xDF: volume/loop fields
Add-U32 3579545               # 0xE0 GA20 clock (libvgm's header slot for DEVID_GA20)
while ($bytes.Count -lt 0x100) { $bytes.Add(0) }

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- ROM ------------------------------------------------------------------------------------------
function Add-RomBlock([uint32]$romOffset, [byte[]]$payload) {
	$bytes.Add(0x67); $bytes.Add(0x66); $bytes.Add(0x93)
	Add-U32 ([uint32](8 + $payload.Length))
	Add-U32 ([uint32]$romSize)
	Add-U32 $romOffset
	$bytes.AddRange($payload)
}

# the tone needs a zero byte after it: that is the sample end marker the chip looks for
$tone = New-Object byte[] ($length + 1)
for ($i = 0; $i -lt $length; $i++) {
	$tone[$i] = if (($i % $cycle) -lt ($cycle / 2)) { 0x50 } else { 0xB0 }
}
$tone[$length] = 0

$seed = 987654321
$noise = New-Object byte[] ($length + 1)
for ($i = 0; $i -lt $length; $i++) {
	$seed = ([long]$seed * 1103515245 + 12345) -band 0x7FFFFFFF
	$noise[$i] = [byte](($seed -shr 16) -band 0xFF)
}
$noise[$length] = 0

Add-RomBlock $start0 $tone
Add-RomBlock $start1 $noise

# --- voices ---------------------------------------------------------------------------------------
Add-Ga20Voice 0 $start0 $speed 0xFF			# tone, full-scale volume
Add-Ga20Voice 1 $start1 $speed 0xFF			# noise

for ($n = 0; $n -lt $notes; $n++) {
	Add-Ga20 0x06 0x02						# voice 0 key on
	Add-Ga20 0x0E 0x02						# voice 1 key on
	Add-Wait $noteSamples
	Add-Ga20 0x06 0x00						# key off
	Add-Ga20 0x0E 0x00
	Add-Wait $noteSamples
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'ga20.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
"  voice 0: G4, {0}-byte cycle at rate {1} -> {2:N0} bytes/s" -f $cycle, $speed, (44100.0 / (256 - $speed))
"  voice 1: percussion, no period -> C4 trigger"
