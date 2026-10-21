# Generates k053260.vgm next to this script: two K053260 voices, four hits each - one melodic,
# one percussion - out of a ROM block.
#
# Why it exists: the K053260 (M4 in docs/desgin/midi-output-design.md) is the third sampled player
# and the first whose registers say how long a note is, but three things about it are easy to get
# wrong and each one is what this asset pins down:
#
#   the key mask        offset 0x28 is a four-bit level register: a voice starts on the rising edge
#                       of its bit and stops when the bit is cleared. Writing a bit that is already
#                       set is not a second note, and writing the voice's own registers is not a
#                       key-on at all.
#   the length register  a non-looping voice plays `length` bytes from `start + 1` (the core's
#                       documented pre-increment) and then stops by itself - no key-off needed.
#                       Both voices here last 1024 bytes, i.e. 36 ms at 28156 bytes a second, so
#                       four notes need four key-ons and nothing else.
#   the pitch            pitch is 12 bits and the rate is `pitch * clock / 262144` bytes a second
#                       at clock 3.579545 MHz. Taken against the 64-byte cycle of voice 0's sample
#                       that is 440 Hz (A4, MIDI 69).
#
# The two voices also cover the two things a sampled voice can be: voice 0 plays a periodic
# waveform and must come out as A4; voice 1 plays noise, which has no period, so it must come out
# as the C4 drum trigger instead of a made-up pitch (these boards put their drums on the same four
# voices as the music - the Vendetta title demo in test_file/K053260 does exactly that).
#
# The ROM block is type 0x92's neighbour: 0x8E is the K053260's entry in libvgm's ROM table
# (vgmplayer_cmdhandler.cpp, _VGM_ROM_CHIPS). The core returns silence until the memory exists
# (`if ((info->mode & 2) && info->rom != NULL)`), and the control register 0x2F has to enable the
# output as well, so a song without either proves nothing about the audio.
#
#   .\make_k053260_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe k053260.vgm 2 44100 --notes

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

# K053260 register write: 0xBA <offset> <data>, the chip's own 8-bit address/data pair.
function Add-K053260([byte]$offset, [byte]$value) {
	$bytes.Add(0xBA)
	$bytes.Add($offset)
	$bytes.Add($value)
}

# One voice's four registers: pitch (12 bits), length (16 bits), start (21 bits), volume (7 bits).
function Add-K053260Voice([int]$voice, [uint16]$pitch, [uint16]$length, [uint32]$start, [byte]$volume) {
	$base = [byte](0x08 + $voice * 8)
	Add-K053260 ([byte]($base + 0)) ([byte]($pitch -band 0xFF))
	Add-K053260 ([byte]($base + 1)) ([byte](($pitch -shr 8) -band 0x0F))
	Add-K053260 ([byte]($base + 2)) ([byte]($length -band 0xFF))
	Add-K053260 ([byte]($base + 3)) ([byte](($length -shr 8) -band 0xFF))
	Add-K053260 ([byte]($base + 4)) ([byte]($start -band 0xFF))
	Add-K053260 ([byte]($base + 5)) ([byte](($start -shr 8) -band 0xFF))
	Add-K053260 ([byte]($base + 6)) ([byte](($start -shr 16) -band 0x1F))
	Add-K053260 ([byte]($base + 7)) $volume
}

$rate = 44100
$clock = 3579545
$noteSamples = [int]($rate / 10)			# 4410 = 100 ms of sound, then 100 ms of silence
$notes = 4
$totalSamples = [uint32]($noteSamples * 2 * $notes)

$pitch = 2062								# 28156 bytes/s at 3.579545 MHz
$cycle = 64									# 28156 / 64 = 440 Hz, A4
$length = 1024								# 16 cycles, and 36 ms of sound
$start0 = 0x0FFF							# the core fetches start + 1 first
$start1 = 0x1FFF
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
Add-U16 0                     # 0x96 C140 banking type, unused here
while ($bytes.Count -lt 0xAC) { $bytes.Add(0) }                    # 0x98 .. 0xAB: C140 and friends
Add-U32 $clock                # 0xAC K053260 clock
while ($bytes.Count -lt 0x100) { Add-U32 0 }                      # volume/loop fields, all zero

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- ROM ------------------------------------------------------------------------------------------
# Two blocks at their own offsets, unioned by MidiSegmentBuilder's assembleRom(): a 64-byte periodic
# waveform for the melodic voice and noise for the percussion one.
function Add-RomBlock([uint32]$romOffset, [byte[]]$payload) {
	$bytes.Add(0x67); $bytes.Add(0x66); $bytes.Add(0x8E)
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

Add-RomBlock 0x1000 $tone
Add-RomBlock 0x2000 $noise

# --- voices ---------------------------------------------------------------------------------------
Add-K053260 0x2F 0x06          # control: sound output enabled
Add-K053260 0x2C 0x24          # pan voices 0/1: both centre (pan 0 would be silent)
Add-K053260Voice 0 $pitch $length $start0 0x7F
Add-K053260Voice 1 $pitch $length $start1 0x7F

for ($n = 0; $n -lt $notes; $n++) {
	Add-K053260 0x28 0x03      # key mask: both voices on a rising edge
	Add-Wait $noteSamples
	Add-K053260 0x28 0x00      # both off
	Add-Wait $noteSamples
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'k053260.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
"  voice 0: A4, {0} bytes at {1:N1} bytes/s" -f $length, ($pitch * $clock / 262144.0)
"  voice 1: percussion, no period -> C4 trigger"
