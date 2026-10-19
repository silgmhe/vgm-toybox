# Generates wswan.vgm next to this script: two seconds of a WonderSwan, one channel per voice type.
#
# The WonderSwan decoder is the only one whose test song has no real track behind it - the files
# this repository carries (test_file/) are Neo Geo, NES, Sega CD and Mega Drive - so the end-to-end
# check (vgm_mixer_test) would otherwise need a hand-made file in a temp folder. Same reasoning as
# make_song.ps1: a real VGM cannot be committed, a generated one can, and the expected notes are
# written down here.
#
#   .\make_wswan_song.ps1
#   ..\..\..\build\plugin\vgm_mixer_test.exe wswan.vgm 3 44100 --notes
#
# Expect four tracks: Ch 1 holds A4 (69) from 0.00 s, Ch 2 fires the voice one-shot (60) at 0.50 s,
# Ch 4 the noise drum (38) at 1.00 s, and Ch 3 walks down from A4 over the last half second (the
# sweep). The layout follows the VGM 1.71 header as libvgm parses it (libs/libvgm/player/vgmplayer.cpp,
# ParseHeader(): data offset at 0x34 relative to 0x34, WonderSwan clock at 0xC0, no loop).

$ErrorActionPreference = 'Stop'

$bytes = New-Object System.Collections.Generic.List[byte]

function Add-U32([uint32]$v) {
	$bytes.Add([byte]($v -band 0xFF))
	$bytes.Add([byte](($v -shr 8) -band 0xFF))
	$bytes.Add([byte](($v -shr 16) -band 0xFF))
	$bytes.Add([byte](($v -shr 24) -band 0xFF))
}

$rate = 44100
$clock = 3072000
$totalSamples = $rate * 2

# --- header, VGM 1.71, 0xF8 bytes, WonderSwan clock at 0xC0 --------------------------------------
$bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('Vgm '))   # 0x00
Add-U32 0                     # 0x04 EOF offset, patched at the end
Add-U32 0x171                 # 0x08 version
for ($i = 0; $i -lt 0x0C; $i++) { $bytes.Add(0) }                 # 0x0C SN76489 / YM2413 clocks
Add-U32 $totalSamples         # 0x18 total samples
Add-U32 0                     # 0x1C loop offset (0 = no loop)
Add-U32 0                     # 0x20 loop samples
Add-U32 $rate                 # 0x24 rate
for ($i = 0; $i -lt 0x0C; $i++) { $bytes.Add(0) }                 # 0x28 flags, YM2612 clock
Add-U32 0xC4                  # 0x34 data offset, relative to 0x34 -> data starts at 0xF8
for ($i = 0; $i -lt 0x84; $i++) { $bytes.Add(0) }                 # 0x38 .. 0xBB unused clocks
Add-U32 0                     # 0xBC extra header offset (none)
Add-U32 $clock                # 0xC0 WonderSwan clock
for ($i = 0; $i -lt 0x34; $i++) { $bytes.Add(0) }                 # 0xC4 .. data offset

if ($bytes.Count -ne 0xF8) { throw ("header is {0} bytes, expected 0xF8" -f $bytes.Count) }

# --- data ----------------------------------------------------------------------------------------
function Add-Reg([byte]$reg, [byte]$value) {
	$bytes.Add(0xBC)      # WonderSwan register write
	$bytes.Add([byte]($reg - 0x80))   # the command stores the address from 0x00, the chip's I/O starts at 0x80
	$bytes.Add($value)
}

function Add-Wait([int]$samples) {
	$bytes.Add(0x61)
	$bytes.Add([byte]($samples -band 0xFF))
	$bytes.Add([byte](($samples -shr 8) -band 0xFF))
}

$half = $rate / 2

# Ch 1: A4 for the whole song - 3072000 / (32 * (2048 - 1830)) = 440.4 Hz
Add-Reg 0x80 0x26             # divider low
Add-Reg 0x81 0x07             # divider high
Add-Reg 0x88 0xFF             # volume 15 on both sides
Add-Reg 0x90 0x01             # channel 1 on

Add-Wait $half

# Ch 2: the voice stream, one trigger per DMA burst
Add-Reg 0x94 0xFF             # PCM volume
Add-Reg 0x90 0x23             # channel 2 on, voice on
Add-Reg 0x89 0x80             # a sound DMA byte - the trigger

Add-Wait $half

# Ch 4: noise, divider 0 = the slowest generator rate, hence the base drum key
Add-Reg 0x86 0x00
Add-Reg 0x87 0x00
Add-Reg 0x8B 0xFF             # volume 15 on both sides
Add-Reg 0x90 0x89             # channel 1, channel 4 and the noise generator on

Add-Wait $half

# Ch 3: a sweep, one step of -1 every ~118 samples (0x8D = 0)
Add-Reg 0x84 0x26             # divider low, also the start of the sweep
Add-Reg 0x85 0x07
Add-Reg 0x8A 0xFF
Add-Reg 0x8C 0xFF             # step -1
Add-Reg 0x8D 0x00             # the fastest sweep interval
Add-Reg 0x90 0xCD             # channel 3 on, sweep on, the rest unchanged

Add-Wait $half

$bytes.Add(0x66) # end of sound data

$eof = $bytes.Count - 4
$bytes[4] = [byte]($eof -band 0xFF)
$bytes[5] = [byte](($eof -shr 8) -band 0xFF)
$bytes[6] = [byte](($eof -shr 16) -band 0xFF)
$bytes[7] = [byte](($eof -shr 24) -band 0xFF)

$out = Join-Path $PSScriptRoot 'wswan.vgm'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, {2} samples = {3:N1} s)" -f $out, $bytes.Count, $totalSamples, ($totalSamples / $rate)
