# Generates ymz280b.vgm next to this script: four sample hits from a YMZ280B voice, with an
# audible 8-bit PCM waveform behind them.
#
# Why it exists: the YMZ280B decoder (M3 in docs/desgin/midi-output-design.md) reports one-shot
# triggers, and a trigger can only be checked end to end against a chip that actually makes a
# sound - `[13]` compares the mixer's level with the MIDI side. Two chip details drive the layout:
#
#   * the core returns silence until a ROM data block has allocated its memory, so the file carries
#     a 0x67 block of type 0x86 (libvgm's _VGM_ROM_CHIPS entry for the chip);
#   * a register write is one VGM command: "5D <reg> <data>". The chip's byte-wide bus needs an
#     offset-0 select / offset-1 write pair, but VGM carries the resolved register number in `aa`
#     (verified against real rips: 0x5D 0x0D 0x20, 0x5D 0x6C 0xBD, ...), and the core re-creates
#     that latch internally.
#
# Voice 0 is set up for 8-bit PCM (mode 2, bits 5-6 of register 0x01), sample 0..0x1000 of the ROM,
# full level, and keyed on four times for 100 ms each. Global key-on enable (register 0x80 bit 7)
# has to be set first, or the core ignores every key-on.
#
#   .\make_ymz280b_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe ymz280b.vgm 2 44100 --notes

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

function Add-Reg([byte]$reg, [byte]$value) {
	$bytes.Add(0x5D); $bytes.Add($reg); $bytes.Add($value)
}

$rate = 44100
$noteSamples = [int]($rate / 10)			# 4410 = 100 ms of sound, then 100 ms of silence
$notes = 4
$totalSamples = [uint32]($noteSamples * 2 * $notes)
$clock = 16934400

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
while ($bytes.Count -lt 0x68) { Add-U32 0 }                       # 0x40 .. 0x67: unused clocks
Add-U32 $clock                # 0x68 YMZ280B clock (libvgm's _CHIPCLK_OFS entry for it)
while ($bytes.Count -lt 0x100) { Add-U32 0 }

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- data -----------------------------------------------------------------------------------------
# The sample: 8-bit signed PCM, a square wave. Without a ROM block the core stays silent.
# libvgm counts the block length from after the 4-byte length field, i.e. it includes the 8-byte
# memSize/dataOfs header and then advances by that length - so memSize + dataOfs + sample has to be
# exactly the value written in the length field, or the parser resumes in the middle of the sample
# and every following command is off by a byte (that is how the register writes first came out
# shifted here). The sample is 0x1000 - 8 for that reason.
$blockSize = 0x1000
$sampleSize = $blockSize - 8
$bytes.Add(0x67); $bytes.Add(0x66); $bytes.Add(0x86)
Add-U32 ([uint32]$blockSize)
Add-U32 ([uint32]$blockSize)           # memSize
Add-U32 0                              # dataOfs
for ($i = 0; $i -lt $sampleSize; $i++) {
	$bytes.Add($(if (($i % 16) -lt 8) { 0x40 } else { 0xC0 }))
}

Add-Reg 0xFF 0x80             # 0xFF: bit 7 key-on enable; bit 6 stays 0, the ROM is read from the
                              # data block, not through an external handler (which does not exist here)
Add-Reg 0x20 0x00             # start address high
Add-Reg 0x40 0x00             # start address middle
Add-Reg 0x60 0x00             # start address low
Add-Reg 0x23 0x00             # stop address high
Add-Reg 0x43 0x08             # stop address middle -> 0x1000
Add-Reg 0x63 0x00             # stop address low
Add-Reg 0x03 0x08             # pan: middle
Add-Reg 0x02 0xFF             # total level: audible
Add-Reg 0x00 0x00             # pitch low
Add-Reg 0x01 0xC1             # pitch high bit + mode 2 (8-bit PCM), key off

for ($n = 0; $n -lt $notes; $n++) {
	Add-Reg 0x01 0xC1         # key on (bit 7) with the same mode and pitch
	Add-Wait $noteSamples
	Add-Reg 0x01 0x41         # key off, mode and pitch unchanged
	Add-Wait $noteSamples
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'ymz280b.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s, {4} notes, {5} byte sample)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate), $notes, $sampleSize
