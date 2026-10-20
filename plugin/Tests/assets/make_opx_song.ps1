# Generates opx.vgm next to this script: two four-operator YMF271 groups in FM mode, four notes
# each, one group an octave above the other.
#
# Why it exists: the YMF271 decoder (M3 in docs/desgin/midi-output-design.md) is tested in
# vgm_chip_test against register streams this repository writes itself. That proves the decoder
# agrees with its own reading of the chip; it does not prove the file path - the 0xD1 command, the
# chip instance in bit 7 of the port byte, the bank left in the low seven bits, and a track that
# the mixer can mute - works end to end. `[13]` compares what the mixer hears with what the MIDI
# side says, so the song has to make the chip sound.
#
# The chip: a group is four slots in four banks. Under sync mode 0 (the reset default) a write to
# bank 0 copies the synced registers (key-on, pitch, algorithm, levels) into the other three banks,
# so the pitch and the key-on only travel on port 0. Total level is per slot rather than synced and
# is written on all four banks, otherwise every operator keeps the 0x7F silence default.
#
# The notes: block 4 and fns 0x51C at 16.934 MHz is 440.25 Hz (the core's base rate is clock / 384 =
# 44100, and the phase step divides by 2^28), i.e. A4 on group 0; block 5 gives A5 on group 1.
#
#   .\make_opx_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe opx.vgm 2 44100 --notes

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

# YMF271 register write: 0xD1 <chip 1 bit | bank 7 bits> <address> <data>. The address's low nibble
# is the group, and 3/7/11/15 are holes (ymf271.c's fm_tab), so the two groups here are 0 and 1.
function Add-Opx([byte]$bank, [byte]$address, [byte]$value) {
	$bytes.Add(0xD1)
	$bytes.Add($bank)
	$bytes.Add($address)
	$bytes.Add($value)
}

# ROM data block (0x67 0x66 <type> <len> <memSize> <dataOfs> <data...>). Type 0x85 is the YMF271's
# external sample memory, and the core mutes every group - the FM ones included - while that memory
# has not been allocated (`ymf271_update` skips a group whose chip->mem_base is NULL), so a song that
# only wants FM still has to hand the chip a ROM block or it plays silence.
function Add-RomBlock([byte]$type, [uint32]$memSize, [uint32]$dataOfs, [byte[]]$data) {
	$bytes.Add(0x67)
	$bytes.Add(0x66)
	$bytes.Add($type)
	Add-U32 ([uint32]($data.Length + 8))
	Add-U32 $memSize
	Add-U32 $dataOfs
	$bytes.AddRange($data)
}

$rate = 44100
$noteSamples = 8820					# 200 ms of sound
$gapSamples = 4410					# 100 ms of silence
$notes = 4
$totalSamples = [uint32](($noteSamples + $gapSamples) * $notes)
$clock = 16934400

# --- header ---------------------------------------------------------------------------------------
$bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('Vgm '))   # 0x00
Add-U32 0                     # 0x04 EOF offset, patched at the end
Add-U32 0x171                 # 0x08 version 1.71 (the YMF271 clock field arrived in 1.51)
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
Add-U32 0                     # 0x40 RF5C68 clock
Add-U32 0                     # 0x44 YM2203 clock
Add-U32 0                     # 0x48 YM2608 clock
Add-U32 0                     # 0x4C YM2610 clock
Add-U32 0                     # 0x50 YM3812 clock
Add-U32 0                     # 0x54 YM3526 clock
Add-U32 0                     # 0x58 Y8950 clock
Add-U32 0                     # 0x5C YMF262 clock
Add-U32 0                     # 0x60 YMF278B clock
Add-U32 $clock                # 0x64 YMF271 clock (libvgm's _CHIPCLK_OFS entry for it)
Add-U32 0                     # 0x68 YMZ280B clock
while ($bytes.Count -lt 0x100) { $bytes.Add(0) }                  # remaining clock/volume fields, all zero

if ($bytes.Count -ne 0x100) { throw ("header is {0} bytes, expected 0x100" -f $bytes.Count) }

# --- data -----------------------------------------------------------------------------------------
# 16 bytes of ROM is enough: the FM groups never read it, they only need the memory to exist.
Add-RomBlock 0x85 0x1000 0 (New-Object byte[] 16)

function Setup-OpxGroup([byte]$group, [byte]$block, [byte]$fnsLow) {
	# multiple 1 and total level 0 on every operator: a modulator at 0x7F is silent, and so is a
	# carrier, and which one carries depends on the algorithm.
	# 0x5 and 0x8 are per slot rather than synced, so they go to all four banks:
	#   attack rate 31  - a key-on starts the envelope at -60 dB, and at rate 0 it never rises,
	#                     which is silence even though the core acknowledges the key-on
	#   release rate 15 - the key-off has to bring the volume down, or the note only ends when
	#                     the stream does
	#   sustain level 0 - the high nibble of 0x8, no extra decay after the attack
	# total level 0x08 rather than 0x00: two groups at full scale add up past 1.0, and a clipped
	# reference WAV says nothing about whether the chip is distorting.
	for ($bank = 0; $bank -lt 4; $bank++) {
		Add-Opx $bank ([byte](0x30 -bor $group)) 0x01
		Add-Opx $bank ([byte](0x40 -bor $group)) 0x08
		Add-Opx $bank ([byte](0x50 -bor $group)) 0x1F
		Add-Opx $bank ([byte](0x80 -bor $group)) 0x0F
	}
	Add-Opx 0 ([byte](0xC0 -bor $group)) 0x00		# algorithm 0: only S4 (bank 3) reaches the output
	Add-Opx 0 ([byte](0xA0 -bor $group)) $block		# block in the high nibble, fns bits 8-11 below
	Add-Opx 0 ([byte](0x90 -bor $group)) $fnsLow	# 0x9 latches fns and is what copies it to the banks
}

Setup-OpxGroup 0x00 0x45 0x1C		# block 4 -> 440.25 Hz
Setup-OpxGroup 0x01 0x55 0x1C		# block 5 -> 880.5 Hz

for ($n = 0; $n -lt $notes; $n++) {
	Add-Opx 0 0x00 0x01				# key on group 0
	Add-Opx 0 0x01 0x01				# key on group 1
	Add-Wait $noteSamples
	Add-Opx 0 0x00 0x00				# key off: one note per group per 300 ms
	Add-Opx 0 0x01 0x00
	Add-Wait $gapSamples
}

$bytes.Add(0x66)

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'opx.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s, {4} notes of {5} samples per group)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate), $notes, $noteSamples
