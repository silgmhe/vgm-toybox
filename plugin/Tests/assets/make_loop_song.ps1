# Generates loop.vgm next to this script: one FM voice playing twenty short notes, with the song's
# own loop point declared in the header.
#
# Why it exists: `PlaybackEngineTest` `[12]` checks that MIDI keeps coming when the song loops - the
# event list covers one pass, and the engine folds the play position back into it at every repeat.
# A song without a loop point cannot show that, so the check printed "no loop point in this song -
# loop check skipped" for every asset in the repository and has never actually run (M1.1 in
# docs/desgin/midi-output-design.md).
#
# Layout: 50 ms of lead-in (so the loop point is not zero), then 20 bursts of
# "key on / 50 ms / key off / 50 ms" = 88200 samples = 2 s of loop body. The header declares the
# loop with the two fields libvgm reads (libs/libvgm/player/vgmplayer.cpp, ParseHeader): the loop
# offset at 0x1C, relative to 0x1C, and the loop length in samples at 0x20. Both have to be right -
# a zero length is ignored, and an offset outside the data area is dropped with a warning.
#
# The header is a full 0x100 bytes (VGM 1.61's own data offset), not the 0x40 the other assets use:
# libvgm reads the loop base count from byte 0x7E and the loop modifier from 0x7F, which with a
# 0x40-byte header would be command bytes. A non-zero base count would silently cut the repeats the
# test asks for.
#
#   .\make_loop_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe loop.vgm 2 44100 --notes

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

function Add-Ym([byte]$reg, [byte]$value) {
	$bytes.Add(0x52)
	$bytes.Add($reg)
	$bytes.Add($value)
}

$rate = 44100
$noteSamples = [int]($rate / 20)            # 2205 samples = 50 ms per half note
$leadIn = $noteSamples                      # 50 ms before the loop point
$notes = 20
$loopSamples = [uint32]($noteSamples * 2 * $notes)      # 88200 = 2 s
$totalSamples = [uint32]($leadIn + $loopSamples)
$clock = 7670453

# --- header ---------------------------------------------------------------------------------------
$bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('Vgm '))   # 0x00
Add-U32 0                     # 0x04 EOF offset, patched at the end
Add-U32 0x161                 # 0x08 version 1.61
Add-U32 0                     # 0x0C SN76489 clock
Add-U32 0                     # 0x10 YM2413 clock
Add-U32 0                     # 0x14 GD3 offset
Add-U32 $totalSamples         # 0x18 total samples
Add-U32 0                     # 0x1C loop offset, patched once the data offset is known
Add-U32 $loopSamples          # 0x20 loop samples
Add-U32 $rate                 # 0x24 rate
for ($i = 0; $i -lt 0x04; $i++) { $bytes.Add(0) }                 # 0x28 SN76489 feedback/shift/flags
Add-U32 $clock                # 0x2C YM2612 clock
Add-U32 0                     # 0x30 YM2151 clock
Add-U32 0xCC                  # 0x34 data offset, relative to 0x34 -> data starts at 0x100
Add-U32 0                     # 0x38 Sega PCM clock
Add-U32 0                     # 0x3C Sega PCM interface, unused
# 0x40 .. 0x7B: clocks of the chips this asset does not use
while ($bytes.Count -lt 0x7C) { Add-U32 0 }
$bytes.Add(0)                 # 0x7C volume gain: 0 dB
$bytes.Add(0)                 # 0x7D reserved
$bytes.Add(0)                 # 0x7E loop base: 0 (a repeat count of 2 has to stay 2)
$bytes.Add(0)                 # 0x7F loop modifier: 0
while ($bytes.Count -lt 0x100) { Add-U32 0 }

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- data -----------------------------------------------------------------------------------------
# One FM voice on channel 1, algorithm 4 (all four operators are carriers, so the patch needs no
# thought about which one reaches the output). The notes are the MIDI the loop test counts.
Add-Ym 0x28 0x00                                  # all keys off
foreach ($op in 0..3) {
	$base = $op * 4
	Add-Ym ([byte](0x30 + $base)) 0x00            # DT/MULT
	Add-Ym ([byte](0x40 + $base)) 0x20            # total level: audible
	Add-Ym ([byte](0x50 + $base)) 0x1F            # attack rate: audible at once
	Add-Ym ([byte](0x60 + $base)) 0x00            # decay rate: the note holds
	Add-Ym ([byte](0x70 + $base)) 0x00            # sustain rate
	Add-Ym ([byte](0x80 + $base)) 0x0F            # sustain level 0, release rate 15
}
Add-Ym 0xB0 0x04                                  # feedback 0, algorithm 4
Add-Ym 0xA4 0x22                                  # block 4, f-number high
Add-Ym 0xA0 0x1D                                  # f-number low -> A4

Add-Wait $leadIn                                  # silence before the loop point

$loopStart = $bytes.Count
for ($n = 0; $n -lt $notes; $n++) {
	Add-Ym 0x28 0xF0                              # key on FM 1
	Add-Wait $noteSamples
	Add-Ym 0x28 0x00                              # key off: one note per burst
	Add-Wait $noteSamples
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

# The loop offset is relative to the header field itself, not to the data area.
$loopOfs = [uint32]($loopStart - 0x1C)
$bytes[0x1C] = [byte]($loopOfs -band 0xFF)
$bytes[0x1D] = [byte](($loopOfs -shr 8) -band 0xFF)
$bytes[0x1E] = [byte](($loopOfs -shr 16) -band 0xFF)
$bytes[0x1F] = [byte](($loopOfs -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'loop.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, lead-in {2} samples, loop {3} samples = {4:N1} s starting at 0x{5:X})" -f $out, $bytes.Count, $leadIn, $loopSamples, ($loopSamples / $rate), $loopStart
