# Generates dac.vgm next to this script: 2 s of YM2612 DAC playback, and nothing else.
#
# The DAC channel produces no notes of its own: the sample bytes live in a 0x67 data block (type
# 0x00, the YM2612 PCM bank) and 0x80-0x8F is "write the next byte of that bank to register 0x2A,
# then wait n samples". A reader that skips data blocks (which this one did) therefore has a DAC
# track with audio and no MIDI at all - the M1.2 gap in docs/desgin/midi-output-design.md.
#
# The stream writes one byte every 15 samples, which is the chip's own rate in the songs that use
# it: 5880 bytes fill 88200 samples = 2 s. The decoder reports at most one DAC trigger per 50 ms,
# so a passing run shows roughly 40 notes on "DAC", not 5880.
#
#   .\make_dac_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe dac.vgm 2 44100 --notes
#
# The header layout follows the VGM 1.61 offsets libvgm reads (libs/libvgm/player/vgmplayer.cpp,
# ParseHeader(): YM2612 clock at 0x2C, data offset at 0x34).

$ErrorActionPreference = 'Stop'

$bytes = New-Object System.Collections.Generic.List[byte]

function Add-U32([uint32]$v) {
	$bytes.Add([byte]($v -band 0xFF))
	$bytes.Add([byte](($v -shr 8) -band 0xFF))
	$bytes.Add([byte](($v -shr 16) -band 0xFF))
	$bytes.Add([byte](($v -shr 24) -band 0xFF))
}

$rate = 44100
$step = 15                                  # samples per DAC byte: 0x8F
$bankSize = [int](($rate * 2) / $step)      # 2 s of stream
$totalSamples = [uint32]($bankSize * $step)
$clock = 7670453

# --- header ---------------------------------------------------------------------------------------
$bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('Vgm '))   # 0x00
Add-U32 0                     # 0x04 EOF offset, patched at the end
Add-U32 0x161                 # 0x08 version 1.61
Add-U32 0                     # 0x0C SN76489 clock
Add-U32 0                     # 0x10 YM2413 clock
Add-U32 0                     # 0x14 GD3 offset
Add-U32 $totalSamples         # 0x18 total samples
Add-U32 0                     # 0x1C loop offset (0 = no loop)
Add-U32 0                     # 0x20 loop samples
Add-U32 $rate                 # 0x24 rate
for ($i = 0; $i -lt 0x04; $i++) { $bytes.Add(0) }                 # 0x28 SN76489 feedback/shift/flags
Add-U32 $clock                # 0x2C YM2612 clock
Add-U32 0                     # 0x30 YM2151 clock
Add-U32 0x0C                  # 0x34 data offset, relative to 0x34 -> data starts at 0x40
Add-U32 0                     # 0x38 Sega PCM clock
Add-U32 0                     # 0x3C Sega PCM interface, unused

if ($bytes.Count -ne 0x40) { throw ("header is {0} bytes, expected 0x40" -f $bytes.Count) }

# --- data -----------------------------------------------------------------------------------------
# The stream alternates 50 ms of samples with 50 ms of 0x80: the decoder reports at most one DAC
# trigger per 50 ms, so a burst is one trigger and the silence after it closes the note. That makes
# the expected number of notes countable (one per 100 ms) instead of "at least one", and it is how
# the DAC is used in songs: a drum hit, then nothing.
$burst = [int]($rate / 20)                # 2205 samples = 50 ms
$burstBytes = [int]($burst / $step)       # 147 bytes
$periodBytes = 2 * $burstBytes

function Add-Ym([byte]$reg, [byte]$value) {
	$bytes.Add(0x52)
	$bytes.Add($reg)
	$bytes.Add($value)
}

# One FM voice on channel 1, held for the whole song. Algorithm 4 makes all four operators
# carriers, so the patch needs no thinking about which one reaches the output: the point of this
# asset is the DAC stream. Without a single FM note the MIDI builder would have no events at all
# and the harness's other sections would have nothing to talk about.
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
Add-Ym 0x28 0xF0                                  # key on FM 1

# DAC on: 0x2B bit 7 routes channel 6 to the sample stream instead of to FM operator output.
Add-Ym 0x2B 0x80

# The data block: 0x67 0x66 <type> <length> <data>. Type 0x00 is the uncompressed YM2612 PCM bank.
$bytes.Add(0x67); $bytes.Add(0x66); $bytes.Add(0x00)
Add-U32 $bankSize
for ($i = 0; $i -lt $bankSize; $i++) {
	# A square wave while the burst lasts, then the chip's silence value: the decoder treats
	# exactly 0x80 as "nothing to report", which is what ends the note.
	if (($i % $periodBytes) -ge $burstBytes) {
		$bytes.Add(0x80)
	} elseif ((($i / 8) % 2) -eq 0) {
		$bytes.Add(0x30)
	} else {
		$bytes.Add(0xD0)
	}
}

# One DAC byte per command, then the wait that goes with it.
for ($i = 0; $i -lt $bankSize; $i++) {
	$bytes.Add(0x8F)
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'dac.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s, {4} DAC bytes)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate), $bankSize
