# Generates c352.vgm next to this script: two C352 voices, an octave apart, each playing four
# 100 ms notes out of a ROM block.
#
# Why it exists: the C352 decoder (M4 in docs/desgin/midi-output-design.md) is the C140's closest
# relative - a playback rate over recorded samples - but it differs in the three places a decoder
# is most likely to get wrong, and each one is what one of the two voices proves:
#
#   the execute write  setting KEYON in a flag register starts nothing; offset 0x202 is what keys
#                      the voices that asked for it. Both voices are keyed by a single 0x202.
#   the word writes    every register is 16-bit and the 0xE1 command carries offset and value as
#                      four bytes, so a decoder that kept only the low halves would read frequency
#                      0 and stay silent.
#   the sample address `(bank << 16) | start`: voice 1 plays bank 1, i.e. 64 KB further into the
#                      ROM, where a different waveform sits.
#
# The two voices also prove the two ways a C352 note ends, which is why this decoder does not fall
# back to the 80 ms one-shot window the RF5C68 needs:
#
#   voice 0  no loop bit: the voice stops when the pointer reaches `wave_end`, so no key-off is
#            ever written and the 100 ms still has to come out as four notes
#   voice 1  loop bit:    the pointer jumps back to `wave_loop`, so only a key-off (KEYOFF plus an
#            execute write) stops it; its four notes are exactly the key-off edges
#
# Pitch: the chip advances the sample by `frequency * clock / (288 * 65536)` bytes a second
# (c352.c), so at 12.288 MHz frequency 43254 is 28160 bytes a second. Taken against the 64-byte
# cycle of voice 0's sample that is 440 Hz (A4, MIDI 69); voice 1's sample repeats every 32 bytes,
# so the same rate is an octave up (A5, MIDI 81). The ROMs hold square waves at exactly the pitch
# the MIDI note names, so what is heard and what is sent agree.
#
# The clock is written the way libvgm reads it: its C352 device config is
# `hdr[0xDC] * 72 / hdr[0xD6]` (vgmplayer.cpp, `DEVID_C352`), so a header clock of 682667 with the
# usual divider of 4 is 12,288,006 Hz - 12.288 MHz to within a rounding step.
#
#   .\make_c352_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe c352.vgm 2 44100 --notes

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

# C352 register write: 0xE1 <offset hi> <offset lo> <data hi> <data lo>. Both halves are
# big-endian; bit 15 of the offset selects the second chip, which this asset keeps clear.
function Add-C352([uint16]$offset, [uint16]$value) {
	$bytes.Add(0xE1)
	$bytes.Add([byte](($offset -shr 8) -band 0x7F))
	$bytes.Add([byte]($offset -band 0xFF))
	$bytes.Add([byte](($value -shr 8) -band 0xFF))
	$bytes.Add([byte]($value -band 0xFF))
}

# One voice register: the eight word registers of voice `voice` start at offset 8 * voice. The
# table is c352.c's C352RegMap: 0 vol_f, 1 vol_r, 2 freq, 3 flags, 4 bank, 5 start, 6 end, 7 loop.
function Add-C352Voice([int]$voice, [int]$register, [uint16]$value) {
	Add-C352 ([uint16]($voice * 8 + $register)) $value
}

$rate = 44100
$clockField = 682667
$clockDivider = 4
$noteSamples = [int]($rate / 10)			# 4410 = 100 ms of sound, then 100 ms of silence
$notes = 4
$totalSamples = [uint32]($noteSamples * 2 * $notes)

# bytes per second = frequency * clock / 18874368 (288 * 65536); see the header comment
$frequency = 43254							# 28160 bytes/s at 12.288 MHz
$cycle0 = 64								# 28160 / 64 = 440 Hz, A4
$cycle1 = 32								# 28160 / 32 = 880 Hz, A5

$length0 = 2816								# 100 ms at 28160 bytes/s
$length1 = 512								# 16 cycles of 32 bytes, i.e. a whole loop
$start0 = 0x0000
$start1 = 0x0000							# in bank 1, i.e. ROM offset 0x10000
$bank1 = 0x0001

$romSize = 0x20000							# 128 KB: bank 1 has to be inside the image

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
while ($bytes.Count -lt 0xD6) { $bytes.Add(0) }                    # 0x98 .. 0xD5: unused clocks
$bytes.Add([byte]$clockDivider)                                    # 0xD6 C352 clock divider
while ($bytes.Count -lt 0xDC) { $bytes.Add(0) }                    # 0xD8 X1-010 clock, unused
Add-U32 $clockField           # 0xDC C352 clock
while ($bytes.Count -lt 0x100) { Add-U32 0 }                      # volume/loop fields, all zero

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- data -----------------------------------------------------------------------------------------
# The core returns silence until its memory has been allocated (`if (c->wave == NULL) return;` in
# c352_update), and only a ROM data block does that. Type 0x92 is the C352's entry in libvgm's ROM
# table (vgmplayer_cmdhandler.cpp, _VGM_ROM_CHIPS); the payload is memSize, dataOfs, data.
$bytes.Add(0x67); $bytes.Add(0x66); $bytes.Add(0x92)
Add-U32 ([uint32](8 + $romSize))      # block length: memSize + dataOfs + the data itself
Add-U32 ([uint32]$romSize)            # memSize
Add-U32 0                             # dataOfs

# A square wave at the pitch each voice's MIDI note names: the sign of a sine at the rate the chip
# plays the bytes back at. The rest of the ROM stays 0, which is silence for signed 8-bit PCM.
$rom = New-Object byte[] $romSize
for ($i = 0; $i -lt $length0; $i++) {
	$rom[$start0 + $i] = if (($i % $cycle0) -lt ($cycle0 / 2)) { 0x50 } else { 0xB0 }
}
for ($i = 0; $i -lt $length1; $i++) {
	$rom[0x10000 + $start1 + $i] = if (($i % $cycle1) -lt ($cycle1 / 2)) { 0x50 } else { 0xB0 }
}
$bytes.AddRange($rom)

# --- voices ---------------------------------------------------------------------------------------
foreach ($voice in 0, 1) {
	Add-C352Voice $voice 0 0xFFFF         # front volume: left and right at full
	Add-C352Voice $voice 1 0xFFFF         # rear volume: the same
	Add-C352Voice $voice 2 $frequency
}
Add-C352Voice 0 4 0x0000                  # voice 0: bank 0
Add-C352Voice 0 5 $start0
Add-C352Voice 0 6 ($start0 + $length0)    # end: the sample's own size - it ends the note
Add-C352Voice 1 4 $bank1                  # voice 1: bank 1, ROM offset 0x10000
Add-C352Voice 1 5 $start1
Add-C352Voice 1 6 ($start1 + $length1)
Add-C352Voice 1 7 $start1                 # loop back to the sample start

for ($n = 0; $n -lt $notes; $n++) {
	Add-C352Voice 0 3 0x4000              # voice 0: key on, no loop
	Add-C352Voice 1 3 0x4002              # voice 1: key on with loop
	Add-C352 0x0202 0x0000                # the execute write that actually starts both
	Add-Wait $noteSamples
	Add-C352Voice 1 3 0x2002              # voice 1: key off, loop still set
	Add-C352 0x0202 0x0000
	Add-Wait $noteSamples
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'c352.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
"  real clock: {0:N0} Hz (header {1} / divider {2})" -f ($clockField * 72 / $clockDivider), $clockField, $clockDivider
"  voice 0: A4, {0} bytes at {1:N1} bytes/s" -f $length0, ($frequency * 12288006.0 / 18874368.0)
"  voice 1: A5, bank 1, looped to the key-off"
