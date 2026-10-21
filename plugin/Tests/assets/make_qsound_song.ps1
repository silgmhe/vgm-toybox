# Generates qsound.vgm next to this script: two QSound voices, four notes each - one melodic, one
# percussion - played out of a ROM block.
#
# Why it exists: the QSound (M4 in docs/desgin/midi-output-design.md) is the fifth sampled player and
# the one with no key bit at all. Three things about it are easy to get wrong, and this asset pins
# down one of each:
#
#   the key              a voice sounds while its rate and its volume are both non-zero; there is no
#                        key-on register. The driver writes the start/end/volume and then the rate,
#                        and silences the voice by writing the rate back to zero. A decoder looking
#                        for a key bit plays nothing (this asset produced silence until the rising
#                        edge of the rate was used); one that ignores the zero write never stops.
#   the bank register    offset (n<<3)+0 writes the bank of voice n+1, not of voice n (the core's
#                        init_register_map). Both voices here stay in bank 0, so the value only has
#                        to arrive, but a decoder that gives it to the wrong voice reads the wrong
#                        sample as soon as a driver banks.
#   the pitch            the rate is 4.12 fixed point added to a 28-bit position, so a voice
#                        advances rate/4096 bytes per output sample at clock/2/1248. With the VGM's
#                        4 MHz clock normalised to 60 MHz that is 24038 bytes a second at rate
#                        0x1000, and a 55-byte cycle is 437 Hz - A4.
#
# The two voices also cover the two things a sampled voice can be: voice 0 plays a periodic waveform
# and must come out as A4; voice 1 plays noise, which has no period, so it must come out as the C4
# drum trigger rather than a pitch the correlator invented (Street Fighter II's drum kit lives on
# these same sixteen voices).
#
# The ROM block type is 0x8F - the QSound's entry in libvgm's ROM table (vgmplayer_cmdhandler.cpp,
# _VGM_ROM_CHIPS).
#
#   .\make_qsound_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe qsound.vgm 2 44100 --notes

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

# QSound register write: 0xC4 <value high> <value low> <offset>.
function Add-QSound([byte]$offset, [uint16]$value) {
	$bytes.Add(0xC4)
	$bytes.Add([byte](($value -shr 8) -band 0xFF))
	$bytes.Add([byte]($value -band 0xFF))
	$bytes.Add($offset)
}

# One voice: pan centre, start, end, no loop, full-scale volume - and no rate, because the rate write
# is what starts (and stops) the voice.
function Add-QSoundVoice([int]$voice, [uint16]$start, [uint16]$end) {
	$base = [byte]($voice * 8)
	Add-QSound ([byte](0x80 + $voice)) ([uint16]0x0120)		# pan centre
	Add-QSound ([byte]($base + 1)) $start					# start
	Add-QSound ([byte]($base + 5)) $end						# end
	Add-QSound ([byte]($base + 4)) ([uint16]0x0000)			# no loop
	Add-QSound ([byte]($base + 6)) ([uint16]0x4000)			# volume, full scale
}

$rate = 44100
$noteSamples = [int]($rate / 10)
$notes = 4
$totalSamples = [uint32]($noteSamples * 2 * $notes)

$pitchRate = 0x1000								# one byte per output sample
$cycle = 55										# 24038 / 55 = 437 Hz, A4
$length = 2048									# 85 ms
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
while ($bytes.Count -lt 0xB4) { $bytes.Add(0) }                    # 0x98 .. 0xB3: MSM6295/C140/K053260
Add-U32 4000000               # 0xB4 QSound clock (the classic form; libvgm normalises it to 60 MHz)
while ($bytes.Count -lt 0x100) { Add-U32 0 }                      # 0xB8 .. 0xFF: volume/loop fields

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- ROM ------------------------------------------------------------------------------------------
# Two blocks at their own offsets, unioned by the builder's assembleRom(): a 55-byte periodic
# waveform for the melodic voice and noise for the percussion one.
function Add-RomBlock([uint32]$romOffset, [byte[]]$payload) {
	$bytes.Add(0x67); $bytes.Add(0x66); $bytes.Add(0x8F)
	Add-U32 ([uint32](8 + $payload.Length))
	Add-U32 ([uint32]$romSize)
	Add-U32 $romOffset
	$bytes.AddRange($payload)
}

$tone = New-Object byte[] $length
for ($i = 0; $i -lt $length; $i++) {
	$tone[$i] = if (($i % $cycle) -lt ($cycle / 2)) { 0x50 } else { 0xB0 }
}

$seed = 987654321
$noise = New-Object byte[] $length
for ($i = 0; $i -lt $length; $i++) {
	$seed = ([long]$seed * 1103515245 + 12345) -band 0x7FFFFFFF
	$noise[$i] = [byte](($seed -shr 16) -band 0xFF)
}

Add-RomBlock $start0 $tone
Add-RomBlock $start1 $noise

# --- voices ---------------------------------------------------------------------------------------
Add-QSoundVoice 0 $start0 ([uint16]($start0 + $length))
Add-QSoundVoice 1 $start1 ([uint16]($start1 + $length))

for ($n = 0; $n -lt $notes; $n++) {
	Add-QSound 0x02 $pitchRate		# voice 0: rate -> key on
	Add-QSound 0x0A $pitchRate		# voice 1
	Add-Wait $noteSamples
	Add-QSound 0x02 0x0000			# rate 0 -> key off
	Add-QSound 0x0A 0x0000
	Add-Wait $noteSamples
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'qsound.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
"  voice 0: A4, {0}-byte cycle at rate 0x{1:X} -> {2:N0} bytes/s" -f $cycle, $pitchRate, (60000000.0 / 10223616.0 * $pitchRate)
"  voice 1: percussion, no period -> C4 trigger"
