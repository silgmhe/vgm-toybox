# Generates x1.vgm next to this script: four notes from an X1-010 channel, with an audible
# wavetable behind them.
#
# Why it exists: the X1-010 decoder (M2 in docs/desgin/midi-output-design.md) can only be checked
# end to end by a song whose chip actually makes a sound - `[13]` compares what the mixer hears with
# what the MIDI side says, and a silent chip proves nothing. The chip's wavetable and envelope
# tables are ordinary memory in the register space (libs/libvgm/emu/cores/x1_010.c: waveform `n` is
# at 0x1000 + 128 * n, envelope `n` at 128 * n), so they can be uploaded with the same 0xC8
# register writes as the notes - no data block needed.
#
# The song: 128 bytes of square wave at 0x1000, 128 bytes of "full volume" envelope at 0x80, then
# four 100 ms notes on channel 1 at A4 (6.10 pitch 0x0735 at 16 MHz = 439.9 Hz).
#
#   .\make_x1_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe x1.vgm 2 44100 --notes

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

# X1-010 register write: 0xC8 <offset hi> <offset lo> <data>
function Add-X1([uint16]$offset, [byte]$value) {
	$bytes.Add(0xC8)
	$bytes.Add([byte](($offset -shr 8) -band 0xFF))
	$bytes.Add([byte]($offset -band 0xFF))
	$bytes.Add($value)
}

$rate = 44100
$noteSamples = [int]($rate / 10)			# 4410 = 100 ms of sound, then 100 ms of silence
$notes = 4
$totalSamples = [uint32]($noteSamples * 2 * $notes)
$clock = 16000000

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
while ($bytes.Count -lt 0xD8) { Add-U32 0 }                       # 0x40 .. 0xD7: unused clocks
Add-U32 $clock                # 0xD8 X1-010 clock (libvgm's _CHIPCLK_OFS entry for it)
while ($bytes.Count -lt 0x100) { Add-U32 0 }                      # volume/loop fields, all zero

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- data -----------------------------------------------------------------------------------------
# The core returns silence until its memory has been allocated (`if (info->rom == NULL) return;` in
# seta_update), and only a ROM data block does that. Type 0x91 is the X1-010's entry in libvgm's
# ROM table (vgmplayer_cmdhandler.cpp, _VGM_ROM_CHIPS), and the payload is memSize, dataOfs, data.
$bytes.Add(0x67); $bytes.Add(0x66); $bytes.Add(0x91)
Add-U32 ([uint32](0x08 + 4))          # block length: header plus the four payload bytes
Add-U32 ([uint32]0x1000)              # memSize: enough for the whole register space
Add-U32 0                             # dataOfs
$bytes.Add(0x00); $bytes.Add(0x00); $bytes.Add(0x00); $bytes.Add(0x00)

# The wavetable: one square wave in block 0. Signed 8-bit, centred on zero.
for ($i = 0; $i -lt 128; $i++) {
	Add-X1 ([uint16](0x1000 + $i)) $(if ($i -lt 64) { 0x30 } else { 0xD0 })
}

# The envelope table in block 1 (0x80..0xFF): both nibbles at 7 is full volume on both sides.
for ($i = 0; $i -lt 128; $i++) {
	Add-X1 ([uint16](0x80 + $i)) 0x77
}

Add-X1 0x0005 0x01            # envelope number 1 -> the table uploaded above
Add-X1 0x0001 0x00            # waveform number 0 -> the square wave uploaded above
Add-X1 0x0002 0x35            # pitch low  (6.10: 0x0735 = 1845 = 439.9 Hz at 16 MHz)
Add-X1 0x0003 0x07            # pitch high
Add-X1 0x0000 0x02            # waveform mode, key off

for ($n = 0; $n -lt $notes; $n++) {
	Add-X1 0x0000 0x03        # key on
	Add-Wait $noteSamples
	Add-X1 0x0000 0x02        # key off: one note per 100 ms
	Add-Wait $noteSamples
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'x1.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s, {4} notes of {5} samples)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate), $notes, $noteSamples
