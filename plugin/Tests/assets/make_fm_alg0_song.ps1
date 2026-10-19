# Generates fm_alg0.vgm next to this script: 2 s of YM2612, three keyed channels (FM 1, FM 2, FM 4)
# whose only carrier is operator 4, because algorithm 0 routes op1 -> op2 -> op3 -> op4 and only the
# last one reaches the output. All three non-carriers are set to total level 0x7F (silent) and the
# carrier to 0x00, so the only thing that makes these channels audible is the total-level register.
#
# That is the register the decoder used to get wrong: in 0x40-0x4F the low two bits are the channel
# and bits 2-3 the operator. Swapped, each write lands on a stranger's channel, the real carrier
# stays at its 0x7F default and the channel plays with no MIDI note (FM5 of "Star Light Zone").
# This file is the smallest stream that separates the two readings: a swapped decoder hears silence
# in all three channels while the chip sounds them.
#
#   .\make_fm_alg0_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe fm_alg0.vgm 2 44100 --notes
#
# Expect: A4 on FM 1 at 0.25 s, C5 on FM 2 at 0.50 s, E5 on FM 4 at 0.75 s; vgm_mixer_test's [13]
# requires exactly one note per keyed channel.
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
$quarter = $rate / 4
$totalSamples = $rate * 2
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
function Add-Ym([byte]$port, [byte]$reg, [byte]$value) {
	if ($port -eq 0) { $bytes.Add(0x52) } else { $bytes.Add(0x53) }
	$bytes.Add($reg)
	$bytes.Add($value)
}

function Add-Wait([int]$samples) {
	$bytes.Add(0x61)
	$bytes.Add([byte]($samples -band 0xFF))
	$bytes.Add([byte](($samples -shr 8) -band 0xFF))
}

# One silent chain with the carrier open. 0x30/0x40/0x50/0x60/0x70/0x80 take the operator in bits
# 2-3 and the channel in bits 0-1, which is the layout the decoder has to agree on.
function Add-CarrierOnlyVoice([byte]$port, [int]$ch) {
	foreach ($op in 0..3) {
		$base = $op * 4
		Add-Ym $port ([byte](0x30 + $base + $ch)) 0x00     # DT/MULT
		# only operator 4 (slot index 3) is a carrier under algorithm 0
		if ($op -eq 3) {
			Add-Ym $port ([byte](0x40 + $base + $ch)) 0x00 # total level: full volume
		} else {
			Add-Ym $port ([byte](0x40 + $base + $ch)) 0x7F # total level: silent
		}
		Add-Ym $port ([byte](0x50 + $base + $ch)) 0x1F     # attack rate: audible at once
		Add-Ym $port ([byte](0x60 + $base + $ch)) 0x00     # decay rate: no decay, the note holds
		Add-Ym $port ([byte](0x70 + $base + $ch)) 0x00     # sustain rate
		Add-Ym $port ([byte](0x80 + $base + $ch)) 0x0F     # sustain level 0, release rate 15
	}

	Add-Ym $port ([byte](0xB0 + $ch)) 0x00                 # feedback 0, algorithm 0
}

# block 4 and the f-number that plays the given note on a 7.670453 MHz YM2612:
# hz = fnum * clock / (144 * 2^(20 - block))
function Add-Pitch([byte]$port, [int]$ch, [int]$fnum) {
	Add-Ym $port ([byte](0xA4 + $ch)) ([byte](0x20 -bor ($fnum -shr 8)))
	Add-Ym $port ([byte](0xA0 + $ch)) ([byte]($fnum -band 0xFF))
}

function Add-Key([byte]$value) {
	Add-Ym 0 0x28 $value
}

# all keys off before the voices are set up, so the first key-on is a rising edge
Add-Key 0x00

Add-CarrierOnlyVoice 0 0
Add-Pitch 0 0 0x21D                                  # A4
Add-CarrierOnlyVoice 0 1
Add-Pitch 0 1 0x284                                  # C5
Add-CarrierOnlyVoice 1 0
Add-Pitch 1 0 0x32B                                  # E5

Add-Wait $quarter
Add-Key 0xF0                                         # FM 1
Add-Wait $quarter
Add-Key 0xF1                                         # FM 2
Add-Wait $quarter
Add-Key 0xF4                                         # FM 4 (port 1, channel 0)
Add-Wait $quarter
Add-Key 0x00                                         # all off
Add-Wait ($totalSamples - 4 * $quarter)

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'fm_alg0.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
