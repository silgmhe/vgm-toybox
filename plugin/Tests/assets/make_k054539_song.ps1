# Generates k054539.vgm next to this script: two K054539 voices, four notes each - one melodic,
# one percussion - played out of a ROM block.
#
# Why it exists: the K054539 (M4 in docs/desgin/midi-output-design.md) is the fourth sampled player
# and the first one whose samples say where they end. Three things about it are easy to get wrong,
# and this asset pins down one of each:
#
#   the marker          a sample stops when the core fetches 0x80 (8-bit PCM), not at a length
#                       register and not at an end address. Both samples here end on that byte, so
#                       four notes per track needs four key-ons and nothing else.
#   the key registers   key-on is a bit in the mask at 0x214 and key-off a bit in the mask at 0x215.
#                       The address is sixteen bits wide (0xD3 carries both halves), so a decoder
#                       that kept only the low byte would see 0x14/0x15 and never start a voice.
#   the pitch           pitch is 24 bits and the chip renders at clock / 384, adding pitch / 65536
#                       units per output sample. At 18.432 MHz one unit per sample is 0x10000, and
#                       56320 units a second over a 128-byte cycle is 440 Hz (A4, MIDI 69).
#
# The two voices also cover the two things a sampled voice can be: voice 0 plays a periodic
# waveform and must come out as A4; voice 1 plays noise, which has no period, so it must come out
# as the C4 drum trigger rather than a pitch the correlator invented (these boards share the eight
# voices between music and drums).
#
# The ROM block type is 0x8C - the K054539's entry in libvgm's ROM table (vgmplayer_cmdhandler.cpp,
# _VGM_ROM_CHIPS). The core returns silence unless the ROM exists and the control register's PCM bit
# (0x22F bit 0) is set, so both are part of the test.
#
#   .\make_k054539_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe k054539.vgm 2 44100 --notes

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

# K054539 register write: 0xD3 <offset high> <offset low> <data>.
function Add-K054539([uint16]$offset, [byte]$value) {
	$bytes.Add(0xD3)
	$bytes.Add([byte](($offset -shr 8) -band 0xFF))
	$bytes.Add([byte]($offset -band 0xFF))
	$bytes.Add($value)
}

# One voice: format byte, no loop, pitch (24 bits), volume (attenuation, 0 = full scale), pan
# centre (0x10) and the start address (24 bits).
function Add-K054539Voice([int]$voice, [uint32]$start, [uint32]$pitch, [byte]$volume) {
	$base = [uint16]($voice * 0x20)
	Add-K054539 ([uint16](0x200 + 2 * $voice)) 0x00          # 8-bit PCM, no reverse
	Add-K054539 ([uint16](0x201 + 2 * $voice)) 0x00          # no loop
	Add-K054539 ([uint16]($base + 0x00)) ([byte]($pitch -band 0xFF))
	Add-K054539 ([uint16]($base + 0x01)) ([byte](($pitch -shr 8) -band 0xFF))
	Add-K054539 ([uint16]($base + 0x02)) ([byte](($pitch -shr 16) -band 0xFF))
	Add-K054539 ([uint16]($base + 0x03)) $volume
	Add-K054539 ([uint16]($base + 0x05)) 0x10                # pan centre
	Add-K054539 ([uint16]($base + 0x08)) 0x00                # loop address (unused without the bit)
	Add-K054539 ([uint16]($base + 0x09)) 0x00
	Add-K054539 ([uint16]($base + 0x0A)) 0x00
	Add-K054539 ([uint16]($base + 0x0C)) ([byte]($start -band 0xFF))
	Add-K054539 ([uint16]($base + 0x0D)) ([byte](($start -shr 8) -band 0xFF))
	Add-K054539 ([uint16]($base + 0x0E)) ([byte](($start -shr 16) -band 0xFF))
}

$rate = 44100
$clock = 18432000
$noteSamples = [int]($rate / 10)			# 4410 = 100 ms of sound, then 100 ms of silence
$notes = 4
$totalSamples = [uint32]($noteSamples * 2 * $notes)

$pitch = 76896								# 56320 units/s at 18.432 MHz
$cycle = 128								# 56320 / 128 = 440 Hz, A4
$length = 2048								# 16 cycles
$start0 = 0x1000
$start1 = 0x2000
$romSize = 0x10000

# --- header ---------------------------------------------------------------------------------------
$bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('Vgm '))   # 0x00
Add-U32 0                     # 0x04 EOF offset, patched at the end
Add-U32 0x161                 # 0x08 version 1.61
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
while ($bytes.Count -lt 0xA0) { $bytes.Add(0) }                    # 0x98 .. 0x9F: C140 clock and friends
Add-U32 $clock                # 0xA0 K054539 clock
while ($bytes.Count -lt 0x100) { Add-U32 0 }                      # 0xA4 .. 0xFF: volume/loop fields

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- ROM ------------------------------------------------------------------------------------------
# Two blocks at their own offsets, unioned by the builder's assembleRom(): a 128-byte periodic
# waveform for the melodic voice and noise for the percussion one, each with an 0x80 end marker.
function Add-RomBlock([uint32]$romOffset, [byte[]]$payload) {
	$bytes.Add(0x67); $bytes.Add(0x66); $bytes.Add(0x8C)
	Add-U32 ([uint32](8 + $payload.Length))
	Add-U32 ([uint32]$romSize)
	Add-U32 $romOffset
	$bytes.AddRange($payload)
}

$tone = New-Object byte[] ($length + 1)
for ($i = 0; $i -lt $length; $i++) {
	$tone[$i] = if (($i % $cycle) -lt ($cycle / 2)) { 0x50 } else { 0xB0 }
}
$tone[$length] = 0x80						# the end marker

$seed = 987654321
$noise = New-Object byte[] ($length + 1)
for ($i = 0; $i -lt $length; $i++) {
	$seed = ([long]$seed * 1103515245 + 12345) -band 0x7FFFFFFF
	$noise[$i] = [byte](($seed -shr 16) -band 0xFF)
}
$noise[$length] = 0x80

Add-RomBlock $start0 $tone
Add-RomBlock $start1 $noise

# --- voices ---------------------------------------------------------------------------------------
Add-K054539 0x22F 0x01			# control: PCM output enabled
Add-K054539Voice 0 $start0 $pitch 0x00
Add-K054539Voice 1 $start1 $pitch 0x00

for ($n = 0; $n -lt $notes; $n++) {
	Add-K054539 0x214 0x03		# key on voices 0 and 1
	Add-Wait $noteSamples
	Add-K054539 0x215 0x03		# key off
	Add-Wait $noteSamples
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'k054539.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
"  voice 0: A4, {0} bytes + marker at {1:N1} units/s" -f $length, ($pitch * $clock / 25165824.0)
"  voice 1: percussion, no period -> C4 trigger"
