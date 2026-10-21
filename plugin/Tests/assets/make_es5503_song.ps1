# Generates es5503.vgm next to this script: two ES5503 oscillators, four notes each - one melodic,
# one percussion - played out of the chip's 128 KB of wave RAM.
#
# Why it exists: the ES5503 (M4 in docs/desgin/midi-output-design.md) is the one *wavetable* chip in
# the family, and its three traps are each a way to get every note wrong:
#
#   the chip rate   the core runs at (clock / 8) / (oscillatorsEnabled + 2), so enabling more
#                   oscillators lowers every pitch. This asset enables thirty-two (0xE1 = 0x3E),
#                   which is 7,159,090 / 8 / 34 = 26,320 samples a second.
#   the step        the accumulator gains `freq` per sample and the index is
#                   `accumulator >> (9 + resolution - tableSize)`, so a voice reads
#                   freq * 26320 / 512 bytes a second when the two size fields agree.
#   the table       a DOC table is a waveform: one cycle here, but drivers upload several (the
#                   Undercover Cops stage theme packs ten cycles into 1024 bytes). The note is the
#                   *table's* repeat rate, so it is the measured cycle that divides the byte rate.
#
# Voice 0 is a 1024-byte single-cycle square at frequency 8,000: 8000 * 26320 / 512 / 1024 = 401.6 Hz
# = G4. Voice 1 is a one-shot noise table, which has no period and must come out as the C4 trigger.
#
# The RAM arrives as a 0x67 block of type 0xE1 (the block type libvgm's _VGM_RAM_CHIPS table names
# for DEVID_ES5503), whose payload starts with the 32-bit address to write at. The register command
# is 0xD5 <port> <register> <value>.
#
#   .\make_es5503_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe es5503.vgm 2 44100 --notes

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

# ES5503 register write: 0xD5 <port> <register> <value>.
function Add-Es5503([byte]$register, [byte]$value) {
	$bytes.Add(0xD5)
	$bytes.Add(0x00)
	$bytes.Add($register)
	$bytes.Add($value)
}

# One oscillator: pointer, size/resolution, frequency, volume - then the control byte that clears
# the halt bit and so keys it on.
function Add-Es5503Voice([int]$osc, [byte]$pointerHigh, [byte]$sizeAndResolution,
						 [uint16]$frequency, [byte]$control) {
	Add-Es5503 ([byte](0x80 + $osc)) $pointerHigh
	Add-Es5503 ([byte](0xC0 + $osc)) $sizeAndResolution
	Add-Es5503 ([byte](0x00 + $osc)) ([byte]($frequency -band 0xFF))
	Add-Es5503 ([byte](0x20 + $osc)) ([byte](($frequency -shr 8) -band 0xFF))
	Add-Es5503 ([byte](0x40 + $osc)) 0xFF
	Add-Es5503 ([byte](0xA0 + $osc)) $control
}

$rate = 44100
$noteSamples = [int]($rate / 5)					# 0.2 s per note
$notes = 4
$totalSamples = [uint32]($noteSamples * $notes)

$clock = 7159090
$frequency = 8000								# 8000 * 26320 / 512 / 1024 = 401.6 Hz, G4
$tableSize = 1024
$ramSize = 0x20000

# --- header ---------------------------------------------------------------------------------------
$bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('Vgm '))   # 0x00
Add-U32 0                     # 0x04 EOF offset, patched at the end
Add-U32 0x171                 # 0x08 version 1.71
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
Add-U32 0                     # 0x3C Sega PCM interface
while ($bytes.Count -lt 0x94) { Add-U32 0 }                       # 0x40 .. 0x93: unused clocks
Add-U16 0                     # 0x94 K051649 clock
$bytes.Add(0x00)              # 0x96 C140 banking type
$bytes.Add(0x00)              # 0x97
while ($bytes.Count -lt 0xCC) { $bytes.Add(0) }                   # 0x98 .. 0xCB
Add-U32 $clock                # 0xCC ES5503 clock (libvgm's _CHIPCLK_OFS slot for DEVID_ES5503)
while ($bytes.Count -lt 0x100) { $bytes.Add(0) }

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- wave RAM -------------------------------------------------------------------------------------
$ram = New-Object byte[] $ramSize
for ($i = 0; $i -lt $tableSize; $i++) {
	$ram[$i] = if ($i -lt ($tableSize / 2)) { 0x50 } else { 0xB0 }	# one cycle, 1024 bytes -> G4
}

$seed = 987654321
for ($i = 0; $i -lt $tableSize; $i++) {
	$seed = ([long]$seed * 1103515245 + 12345) -band 0x7FFFFFFF
	$ram[$tableSize + $i] = [byte](($seed -shr 16) -band 0xFF)		# noise, one-shot -> C4
}

$bytes.Add(0x67); $bytes.Add(0x66); $bytes.Add(0xE1)				# RAM write, 32-bit address
Add-U32 ([uint32](4 + $ramSize))
Add-U32 0															# address 0x0000
$bytes.AddRange($ram)

# --- voices ---------------------------------------------------------------------------------------
Add-Es5503 0xE1 0x3E											# thirty-two oscillators
Add-Es5503Voice 0 0x00 0x12 $frequency 0x00						# table at 0x0000, free-running
Add-Es5503Voice 1 0x04 0x12 $frequency 0x02						# table at 0x0400, one-shot

for ($n = 0; $n -lt $notes; $n++) {
	Add-Es5503 0xA0 0x00										# voice 0: clear halt -> key on
	Add-Es5503 0xA1 0x02										# voice 1: one-shot key on
	Add-Wait ([uint16]($noteSamples / 2))
	Add-Es5503 0xA0 0x01										# voice 0: set halt -> key off
	Add-Wait ([uint16]($noteSamples / 2))
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'es5503.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
"  voice 0: G4, 1024-byte cycle at frequency {0} -> {1:N1} Hz" -f $frequency, ($frequency * (7159090.0 / 8.0 / 34.0) / 512.0 / 1024.0)
"  voice 1: percussion, no period -> C4 trigger"
