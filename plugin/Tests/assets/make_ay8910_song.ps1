# Generates ay8910.vgm next to this script: two seconds of a standalone AY-3-8910, three tones.
#
# Why it exists: the AY is the one chip whose MIDI capability was marked unavailable while its
# decoder worked. Ay8910Decoder has been in the tree since cbaf588 and computes pitch from the
# twelve-bit tone divider; midiCapabilityFor() still said "nothing in this backend decodes the
# SSG", a sentence written before the decoder existed. Nothing in Tests/assets was a standalone
# AY file, so the branch was never exercised and the two never contradicted each other in CI.
#
# The three tones are A4, C5 and E5, the same notes make_ym2608_song.ps1 plays through a linked
# SSG, so the two files are comparable: one AY behind an OPN chip, one AY on its own.
#
#   .\make_ay8910_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe ay8910.vgm 2 44100 --notes
#
#$ErrorActionPreference = 'Stop'

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

# A 0x61 wait carries 16 bits of samples; a two-second gap needs more than one.
function Add-WaitLong([int]$samples) {
	while ($samples -gt 65535) {
		Add-Wait 65535
		$samples = $samples - 65535
	}
	if ($samples -gt 0) { Add-Wait ([uint16]$samples) }
}

# 0xA0 <addr> <value> is the AY8910 register write. Unlike the YM2608's SSG, which reaches the same
# registers through port 0x56, a standalone chip has a command of its own.
function Add-Ay([byte]$addr, [byte]$value) {
	$bytes.Add(0xA0)
	$bytes.Add($addr)
	$bytes.Add($value)
}

$rate = 44100
$clock = 1789773     # the decoder's default when the song names no clock (Ay8910Decoder::kRequirements)
$totalSamples = $rate * 2

# --- header, VGM 1.71, 0xF8 bytes, AY8910 clock at 0x4C -------------------------------------------
$bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('Vgm '))   # 0x00
Add-U32 0                     # 0x04 EOF offset, patched at the end
Add-U32 0x171                 # 0x08 version
for ($i = 0; $i -lt 0x0C; $i++) { $bytes.Add(0) }                 # 0x0C SN76489 / YM2413 / GD3 offset
Add-U32 $totalSamples         # 0x18 total samples
Add-U32 0                     # 0x1C loop offset (0 = no loop)
Add-U32 0                     # 0x20 loop samples
Add-U32 $rate                 # 0x24 rate
for ($i = 0; $i -lt 0x0C; $i++) { $bytes.Add(0) }                 # 0x28 flags, YM2612 / YM2151 clocks
Add-U32 0xC4                  # 0x34 data offset, relative to 0x34 -> data starts at 0xF8
for ($i = 0; $i -lt 0x10; $i++) { $bytes.Add(0) }                 # 0x38 SegaPCM / SPCM / RF5C68 / YM2203
for ($i = 0; $i -lt 0x2C; $i++) { $bytes.Add(0) }                 # 0x48 .. 0x73 the OPN and OPL clocks, unused here
Add-U32 $clock                # 0x74 AY8910 clock
$bytes.Add(0x00)              # 0x78 AY8910 chip type: 0 = AY-3-8910 (1 would be YM2149)
for ($i = 0; $i -lt 0x3F; $i++) { $bytes.Add(0) }                 # 0x79 .. 0xB7
Add-U32 0                     # 0xB8 (unused by this file)
for ($i = 0; $i -lt 0x3C; $i++) { $bytes.Add(0) }                 # 0xBC .. data offset

if ($bytes.Count -ne 0xF8) { throw ("header is {0} bytes, expected 0xF8" -f $bytes.Count) }

# --- the three voices ----------------------------------------------------------------------------
# period = clock / (16 * frequency): A4 440 Hz = 254 = 0x00FE, C5 523 Hz = 214 = 0x00D6,
# E5 659 Hz = 170 = 0x00AA. Low byte first, then the high nibble.
Add-Ay 0x00 0xFE; Add-Ay 0x01 0x00      # channel A
Add-Ay 0x02 0xD6; Add-Ay 0x03 0x00      # channel B
Add-Ay 0x04 0xAA; Add-Ay 0x05 0x00      # channel C

# 0x07: the mixer. Bits 0-2 enable the three tone generators, bits 3-5 the noise ones; 0x38 is
# "three tones, no noise".
Add-Ay 0x07 0x38

# 0x08..0x0A: the amplitudes, 0x0F being full scale.
Add-Ay 0x08 0x0F
Add-Ay 0x09 0x0F
Add-Ay 0x0A 0x0F

Add-WaitLong ($totalSamples - 1600)
Add-Ay 0x08 0x00                        # channel A off, so the last block is not all three
Add-WaitLong 1600

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'ay8910.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
