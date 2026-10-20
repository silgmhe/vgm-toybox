# Generates c140.vgm next to this script: two C140 voices, an octave apart, each playing four
# 100 ms notes out of a ROM block.
#
# Why it exists: the C140 decoder (M3 in docs/desgin/midi-output-design.md) reads a playback rate,
# not a pitch, and its ROM has to be allocated by a data block before the core makes any sound - so
# a register-only asset would prove the decoder and nothing else. `[13]` compares what the mixer
# hears with what the MIDI side says, which needs both.
#
# The two voices prove the two ways a C140 note ends, which is the whole reason this decoder does
# not use the 80 ms one-shot window the RF5C68 needs:
#
#   voice 0  no loop bit: the voice stops when the pointer reaches `end - start`, so no key-off is
#            ever written and the 100 ms still has to come out as four notes
#   voice 16 loop bit:    the pointer wraps to `loop`, so only a mode write without bit 7 stops it;
#            its four notes are exactly the key-off edges, and its registers sit above 0xFF so the
#            16-bit offset of 0xD4 has to be reassembled to reach them at all
#
# Pitch: the chip advances the sample at `frequency * clock / 288` bytes a second (c140.c), so at
# 8 MHz `frequency` 31711 is 13440 bytes a second. Taken against the 8 kHz root rate in Pitch.h
# that is A4 (MIDI 69), and 63422 is 26882 bytes a second, an octave up (A5, MIDI 81). The sample
# data is a square wave at the pitch the MIDI note names, so what is heard and what is sent agree.
#
#   .\make_c140_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe c140.vgm 2 44100 --notes

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

# C140 register write: 0xD4 <offset hi> <offset lo> <data>. The offset is big-endian and bit 7 of
# the high byte selects the second chip, so this asset keeps it clear.
function Add-C140([uint16]$offset, [byte]$value) {
	$bytes.Add(0xD4)
	$bytes.Add([byte](($offset -shr 8) -band 0x7F))
	$bytes.Add([byte]($offset -band 0xFF))
	$bytes.Add($value)
}

$rate = 44100
$clock = 8000000
$noteSamples = [int]($rate / 10)			# 4410 = 100 ms of sound, then 100 ms of silence
$notes = 4
$totalSamples = [uint32]($noteSamples * 2 * $notes)

# bytes per second = frequency * clock / 18874368 (288 * 65536)
$bytesPerSecondPerStep = $clock / 18874368.0
$frequency0 = 31711							# A4: 13440.6 bytes/s
$frequency1 = 63422							# A5: 26881.9 bytes/s, an octave up
$rate0 = $frequency0 * $bytesPerSecondPerStep
$rate1 = $frequency1 * $bytesPerSecondPerStep
$pitch0 = 440.0
$pitch1 = 880.0

# Both samples are 100 ms long: the length in bytes follows from the playback rate, so an octave
# up covers twice as many bytes in the same time
$length0 = 1344
$length1 = 2688
$start0 = 0x1000
$start1 = 0x2000

if (($start0 + $length0) -gt $start1 -or ($start1 + $length1) -gt 0x8000) {
	throw "the two samples overlap or leave the ROM"
}

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
Add-U16 0                     # 0x94 K051649 clock, still unused
Add-U16 0                     # 0x96 C140 banking type: 0 = System 2
while ($bytes.Count -lt 0xA8) { $bytes.Add(0) }                   # 0x98/0x9C/0xA0/0xA4: unused clocks
Add-U32 $clock                # 0xA8 C140 clock (libvgm's _CHIPCLK_OFS entry for it)
while ($bytes.Count -lt 0x100) { Add-U32 0 }                      # volume/loop fields, all zero

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- data -----------------------------------------------------------------------------------------
# The core returns silence until its memory has been allocated (`if (info->pRom == NULL) return;`
# in c140_update), and only a ROM data block does that. Type 0x8D is the C140's entry in libvgm's
# ROM table (vgmplayer_cmdhandler.cpp, _VGM_ROM_CHIPS); the payload is memSize, dataOfs, data.
$romSize = 0x8000
$bytes.Add(0x67); $bytes.Add(0x66); $bytes.Add(0x8D)
Add-U32 ([uint32](8 + $romSize))      # block length: memSize + dataOfs + the data itself
Add-U32 ([uint32]$romSize)            # memSize
Add-U32 0                             # dataOfs

# A square wave at the pitch each voice's MIDI note names: the sign of a sine at the rate the chip
# will play the bytes back at. The rest of the ROM stays 0, which is silence for INT8 PCM.
$rom = New-Object byte[] $romSize
for ($i = 0; $i -lt $length0; $i++) {
	$rom[$start0 + $i] = if ([math]::Sin(2.0 * [math]::PI * $pitch0 * $i / $rate0) -ge 0) { 0x30 } else { 0xD0 }
}
for ($i = 0; $i -lt $length1; $i++) {
	$rom[$start1 + $i] = if ([math]::Sin(2.0 * [math]::PI * $pitch1 * $i / $rate1) -ge 0) { 0x30 } else { 0xD0 }
}
$bytes.AddRange($rom)

# --- voices ---------------------------------------------------------------------------------------
Add-C140 0x0000 0xFF                  # voice 0 right volume
Add-C140 0x0001 0xFF                  # voice 0 left volume
Add-C140 0x0004 0x00                  # bank 0
Add-C140 0x0006 ([byte](($start0 -shr 8) -band 0xFF))   # start high
Add-C140 0x0007 ([byte]($start0 -band 0xFF))            # start low
Add-C140 0x0008 ([byte](($start0 + $length0 -shr 8) -band 0xFF))  # end high
Add-C140 0x0009 ([byte](($start0 + $length0) -band 0xFF))         # end low
Add-C140 0x0002 ([byte](($frequency0 -shr 8) -band 0xFF))         # frequency high
Add-C140 0x0003 ([byte]($frequency0 -band 0xFF))                  # frequency low

# Voice 1 is channel 16, not 1: its registers start at offset 0x100, so 0xD4's high offset byte is
# non-zero and the split across the write's port and register bytes has to be reassembled for the
# note to appear at all - a voice below 16 would pass even if the high half were dropped.
Add-C140 0x0100 0xFF                  # voice 16 right volume
Add-C140 0x0101 0xFF                  # voice 16 left volume
Add-C140 0x0104 0x00                  # bank 0
Add-C140 0x0106 ([byte](($start1 -shr 8) -band 0xFF))
Add-C140 0x0107 ([byte]($start1 -band 0xFF))
Add-C140 0x0108 ([byte](($start1 + $length1 -shr 8) -band 0xFF))
Add-C140 0x0109 ([byte](($start1 + $length1) -band 0xFF))
Add-C140 0x010A ([byte](($start1 -shr 8) -band 0xFF))    # loop back to the sample start
Add-C140 0x010B ([byte]($start1 -band 0xFF))
Add-C140 0x0102 ([byte](($frequency1 -shr 8) -band 0xFF))
Add-C140 0x0103 ([byte]($frequency1 -band 0xFF))

for ($n = 0; $n -lt $notes; $n++) {
	Add-C140 0x0005 0x80             # voice 0: key on, no loop - the end address ends the note
	Add-C140 0x0105 0x90             # voice 16: key on with loop
	Add-Wait $noteSamples
	Add-C140 0x0105 0x10             # voice 16: loop bit without the key bit is a key-off
	Add-Wait $noteSamples
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'c140.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
"  voice 0: A4 at {0:N1} bytes/s, {1} bytes = {2:N2} s" -f $rate0, $length0, ($length0 / $rate0)
"  voice 1: A5 at {0:N1} bytes/s, {1} bytes = {2:N2} s" -f $rate1, $length1, ($length1 / $rate1)
