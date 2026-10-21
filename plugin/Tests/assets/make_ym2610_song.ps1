# Generates ym2610.vgm next to this script: two seconds of a YM2610 (OPNB) with one FM voice per
# half second, the ADPCM-A channels and the Delta-T channel on top.
#
# The YM2610 has no chips/ decoder: its FM half is the same OPN register set as the YM2203 (which
# writeFm already handles), and the percussion is a register-to-one-shot mapping, so the code sits
# in MidiSequenceBuilder::writeYm2610 next to the YM2608. This asset is what proves that path end
# to end - the register writes, the 13 tracks, the ones the non-B variant cannot play and the
# ADPCM triggers - without which only the hand-run mixer test would ever see it.
#
#   .\make_ym2610_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe ym2610.vgm 2 44100 --notes
#
# Expect five tracks with notes and eight without:
#   * track 1  FM 2  A4 (69) at 0.00 s, key off at 0.50 s
#   * track 2  FM 3  C5 (72) at 0.50 s, key off at 1.00 s
#   * tracks 6 and 7  ADPCM-A 1/2, one C4 each at 1.00 s
#   * track 12 ADPCM-B, one C4 at 1.40 s
#   * track 4  FM 5  E5 (76) at 1.50 s, key off at 1.80 s
# FM 1 (track 0) and FM 4 (track 3) stay silent: the non-B variant has no channel there, which is
# why the notes are keyed on 0xF1/0xF2/0xF5/0xF6 (channel bits + the bit-2 group bit) and never on
# 0xF0/0xF4.
#
# The layout follows the VGM 1.71 header as libvgm parses it (libs/libvgm/player/vgmplayer.cpp,
# ParseHeader(): data offset at 0x34 relative to 0x34, YM2610 clock at 0x4C, bit 31 of that clock is
# the B flag, no loop).

$ErrorActionPreference = 'Stop'

$bytes = New-Object System.Collections.Generic.List[byte]

function Add-U32([uint32]$v) {
	$bytes.Add([byte]($v -band 0xFF))
	$bytes.Add([byte](($v -shr 8) -band 0xFF))
	$bytes.Add([byte](($v -shr 16) -band 0xFF))
	$bytes.Add([byte](($v -shr 24) -band 0xFF))
}

$rate = 44100
$clock = 8000000
$totalSamples = $rate * 2

# --- header, VGM 1.71, 0xF8 bytes, YM2610 clock at 0x4C -------------------------------------------
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
Add-U32 0                     # 0x48 YM2608 clock (none)
Add-U32 $clock                # 0x4C YM2610 clock, bit 31 clear = the 4-channel non-B variant
for ($i = 0; $i -lt 0x6C; $i++) { $bytes.Add(0) }                 # 0x50 .. 0xBB the remaining clocks
Add-U32 0                     # 0xBC extra header offset (none)
Add-U32 0                     # 0xC0 WonderSwan clock
for ($i = 0; $i -lt 0x34; $i++) { $bytes.Add(0) }                 # 0xC4 .. data offset

if ($bytes.Count -ne 0xF8) { throw ("header is {0} bytes, expected 0xF8" -f $bytes.Count) }

# --- data ----------------------------------------------------------------------------------------
function Add-Port0([byte]$addr, [byte]$value) {
	$bytes.Add(0x58)      # YM2610 #0 port 0
	$bytes.Add($addr)
	$bytes.Add($value)
}

function Add-Port1([byte]$addr, [byte]$value) {
	$bytes.Add(0x59)      # YM2610 #0 port 1
	$bytes.Add($addr)
	$bytes.Add($value)
}

function Add-Wait([int]$samples) {
	$bytes.Add(0x61)
	$bytes.Add([byte]($samples -band 0xFF))
	$bytes.Add([byte](($samples -shr 8) -band 0xFF))
}

# A four-operator patch whose algorithm 7 makes every operator a carrier: TL 0 and the fastest
# attack, so the channel is audible with nothing but these writes (an OPN powers up at TL 0x7F,
# which is silence). $ch is the channel number inside the port (1..3), $port1 the second register
# bank, which is where channels 4..6 live.
function Add-FmPatch([int]$ch, [bool]$port1) {
	$write = if ($port1) { ${function:Add-Port1} } else { ${function:Add-Port0} }
	$base = 0x30 + $ch
	foreach ($slot in 0, 4, 8, 12) {
		& $write ([byte]($base + $slot))        0x01   # DT / MUL
		& $write ([byte]($base + 0x10 + $slot)) 0x00   # TL, loudest
		& $write ([byte]($base + 0x20 + $slot)) 0x1F   # AR, fastest attack
		& $write ([byte]($base + 0x30 + $slot)) 0x00   # DR
		& $write ([byte]($base + 0x40 + $slot)) 0x00   # SR
		& $write ([byte]($base + 0x50 + $slot)) 0x0F   # SL 0, RR 15
		& $write ([byte]($base + 0x60 + $slot)) 0x00   # SSG-EG
	}
	& $write ([byte](0xB0 + $ch)) 0x07   # FB 0, ALG 7
	& $write ([byte](0xB4 + $ch)) 0xC0   # L + R
}

# f = fnum * clock / (144 * 2^(20 - block)), so fnum = f * 144 * 2^(20 - block) / clock.
# The exponent is 20 - block, not 21: the first version of this script had it one too high and the
# three FM notes came out an octave sharp (A5/C6/E6) while the comments said A4/C5/E5 - the chip was
# right and the expectation was wrong.
function Add-FmPitch([int]$ch, [bool]$port1, [int]$fnum, [int]$block) {
	$write = if ($port1) { ${function:Add-Port1} } else { ${function:Add-Port0} }
	& $write ([byte](0xA0 + $ch)) ([byte]($fnum -band 0xFF))
	& $write ([byte](0xA4 + $ch)) ([byte]((($block -band 7) -shl 3) -bor (($fnum -shr 8) -band 0x07)))
}

$half = $rate / 2
$beat = $rate / 10

# FM channel 2 (track 1): A4 = 440 Hz, fnum 0x207 at block 4
Add-FmPatch 1 $false
Add-FmPitch 1 $false 0x207 4
Add-Port0 0x28 0xF1                   # all four operators on, channel 2

Add-Wait $half

Add-Port0 0x28 0x01                   # key off channel 2
# FM channel 3 (track 2): C5 = 523.3 Hz, fnum 0x269 at block 4
Add-FmPatch 2 $false
Add-FmPitch 2 $false 0x269 4
Add-Port0 0x28 0xF2

Add-Wait $half

Add-Port0 0x28 0x02                   # key off channel 3
Add-Port1 0x00 0x03                   # ADPCM-A: start channels 1 and 2 (tracks 6 and 7)

Add-Wait ($beat * 4)

Add-Port1 0x00 0x80                   # ADPCM-A: stop everything
# Delta-T: DELTA-N is the pitch register (29 is the set's own rate, A4 for the 16-nibble cycle),
# 0x1B is the level - a start with either left at zero is silence
# DELTA-N 8341: (clock/144)/65536 x 8341 = 7,070 nibbles a second, so the 16-nibble tone is A4
Add-Port0 0x19 0x95
Add-Port0 0x1A 0x20
Add-Port0 0x1B 0xFF
Add-Port0 0x10 0x80                   # Delta-T: start (track 12)

Add-Wait $beat

# FM channel 5 (track 4) lives in the second register bank: E5 = 659.3 Hz, fnum 0x30A at block 4
Add-FmPatch 1 $true
Add-FmPitch 1 $true 0x30A 4
Add-Port0 0x28 0xF5                   # channel bits 1, group bit 2 -> FM channel 5

Add-Wait ($beat * 3)

Add-Port0 0x28 0x05                   # key off channel 5

Add-Wait ($beat * 2)

$bytes.Add(0x66) # end of sound data

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'ym2610.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
