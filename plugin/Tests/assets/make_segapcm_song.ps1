# Generates segapcm.vgm next to this script: two Sega PCM voices, four notes each - one melodic, one
# percussion - played out of a ROM block.
#
# Why it exists: the Sega PCM (M4 in docs/desgin/midi-output-design.md) keeps its playback state in
# the register RAM the driver writes through command 0xC0, and three things about it are easy to get
# wrong:
#
#   the address      the 24-bit position's low byte is the chip's own fraction, so the byte address is
#                    (reg 0x85 << 8) | reg 0x84. A start of 0x4000 is mid 0x00, high 0x40.
#   the end          reg 8n+6 counts 256-byte units and the core compares `addr >> 16` against
#                    `reg + 1`, so 0x47 ends the sample at 0x4800 - two kilobytes after 0x4000.
#   the rate         the position advances `pitch` in 1/256-byte units per output sample at
#                    clock/128, so the byte rate is pitch * clock / 32768. At the Galaxy Force II
#                    clock (4,026,987 Hz) pitch 29 is 3563 bytes a second and a 16-byte cycle is A3.
#
# Voice 0 plays the periodic waveform and must come out as A3. Voice 1 plays noise, which has no
# period, so it must come out as the C4 drum trigger - Galaxy Force II's slap bass and its drums ride
# the same sixteen voices.
#
# The ROM block type is 0x80 and the command is 0xC0 (`C0 <offset low> <offset high> <value>`).
#
#   .\make_segapcm_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe segapcm.vgm 2 44100 --notes

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

# Sega PCM: a byte of the register RAM at a 16-bit offset.
function Add-SegaPcm([uint16]$offset, [byte]$value) {
	$bytes.Add(0xC0)
	$bytes.Add([byte]($offset -band 0xFF))
	$bytes.Add([byte](($offset -shr 8) -band 0xFF))
	$bytes.Add($value)
}

# Volume, end, pitch, current address - then the control byte that starts (0x02) or stops (0x01).
function Add-SegaVoice([int]$channel, [uint32]$start, [byte]$pitch, [byte]$endReg, [byte]$control) {
	$low = [uint16]($channel * 8)
	$high = [uint16](0x80 + $channel * 8)
	Add-SegaPcm ($low + 2) 0x7F
	Add-SegaPcm ($low + 3) 0x7F
	Add-SegaPcm ($low + 6) $endReg
	Add-SegaPcm ($low + 7) $pitch
	Add-SegaPcm ($high + 4) ([byte]($start -band 0xFF))
	Add-SegaPcm ($high + 5) ([byte](($start -shr 8) -band 0xFF))
	Add-SegaPcm ($high + 6) $control
}

$rate = 44100
$clock = 4026987
$noteSamples = [int]($rate / 10)
$notes = 4
$totalSamples = [uint32]($noteSamples * 2 * $notes)

$pitch = 29										# 29 * 4026987 / 32768 = 3563 bytes a second
$cycle = 16										# 3563 / 16 = 222.7 Hz, A3
$length = 2048
$start0 = 0x4000
$start1 = 0x5000
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
Add-U32 $clock                # 0x38 Sega PCM clock
Add-U32 0x0D                  # 0x3C Sega PCM bank shift, as Galaxy Force II's header has it
Add-U32 0                     # 0x40 Sega PCM interface / bank mask
while ($bytes.Count -lt 0x100) { Add-U32 0 }

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- ROM ------------------------------------------------------------------------------------------
function Add-RomBlock([uint32]$romOffset, [byte[]]$payload) {
	$bytes.Add(0x67); $bytes.Add(0x66); $bytes.Add(0x80)
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
$end0 = [byte](($start0 + $length) / 256 - 1)		# (reg + 1) * 256 is the end byte address
$end1 = [byte](($start1 + $length) / 256 - 1)

for ($n = 0; $n -lt $notes; $n++) {
	Add-SegaVoice 0 $start0 $pitch $end0 0x02		# start, no loop
	Add-SegaVoice 1 $start1 $pitch $end1 0x02
	Add-Wait $noteSamples
	Add-SegaVoice 0 $start0 $pitch $end0 0x01		# control bit 0 set: key off
	Add-SegaVoice 1 $start1 $pitch $end1 0x01
	Add-Wait $noteSamples
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'segapcm.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
"  voice 0: A3, {0}-byte cycle at pitch {1} -> {2:N0} bytes/s" -f $cycle, $pitch, ($pitch * $clock / 32768.0)
"  voice 1: percussion, no period -> C4 trigger"
