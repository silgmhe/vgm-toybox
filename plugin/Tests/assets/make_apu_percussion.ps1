# Generates apu_percussion.nsf next to this script: the 2A03's two sample-based voices, both
# audible and both moving.
#
# Why it exists: the noise channel's sixteen period settings and the DPCM channel's sample slots
# are numbered into notes so a panel can draw them and a MIDI port can send them - but nothing in
# the corpus ever plays either for long enough to check. A file that holds one period and one
# sample would pass either way; this one steps through four periods and two samples.
#
# How it works: PLAY has no branches at all. It shifts the frame counter down and uses the result
# as a table index, so every 64 frames the noise period advances through the table, and every 128
# frames the DPCM address toggles between the two slots. Writing $4015 on every frame retriggers
# the sample, which is also what keeps the noise's length counter from running out.
#
# DPCM samples have to live at $C000 and up, because $4012 addresses them as ($C000 + value*64).
# The ROM therefore runs to $C040 + 17 bytes - the file is 16 KB of mostly padding, which is the
# price of testing a channel that reads its data from the top of the address space.
#
#   .\make_apu_percussion.ps1
#   ..\..\..\build\plugin\vgm_nsf_probe.exe apu_percussion.nsf --chips
#
# Expect Noise on one of 43..58 and DPCM on 60 or 61.

$ErrorActionPreference = 'Stop'

$rom = New-Object System.Collections.Generic.List[byte]

function Emit([byte[]]$bytes) { $rom.AddRange($bytes) }

function Pad-To([int]$offset) {
	while ($rom.Count -lt $offset) { $rom.Add(0) }
}

# --- INIT at $8000 ------------------------------------------------------------------------------
Emit @(0xA9, 0x1F, 0x8D, 0x15, 0x40)     # LDA #$1F : STA $4015 - enable all five voices
Emit @(0xA9, 0x1F, 0x8D, 0x0C, 0x40)     # LDA #$1F : STA $400C - noise, constant volume 15
Emit @(0xA9, 0x0F, 0x8D, 0x0F, 0x40)     # LDA #$0F : STA $400F - noise length counter
Emit @(0x60)                             # RTS

# --- PLAY at $8010 ------------------------------------------------------------------------------
Pad-To 0x10
$playAddress = 0x8000 + $rom.Count

Emit @(0xE6, 0x00)                       # INC $00     - frame counter
Emit @(0xA5, 0x00)                       # LDA $00

# noise period: (frame >> 6) & 7, one step every 64 frames
foreach ($i in 0..5) { Emit @(0x4A) }    # LSR A x6
Emit @(0x29, 0x07)                       # AND #$07
Emit @(0xAA)                             # TAX
Emit @(0xBD, 0x60, 0x80)                 # LDA noiseperiods,X   ($8060)
Emit @(0x8D, 0x0E, 0x40)                 # STA $400E
Emit @(0xA9, 0x0F, 0x8D, 0x0F, 0x40)     # LDA #$0F : STA $400F - reload the length counter

# DPCM slot: (frame >> 7) & 1, one step every 128 frames
Emit @(0xA5, 0x00)                       # LDA $00
foreach ($i in 0..6) { Emit @(0x4A) }    # LSR A x7
Emit @(0x29, 0x01)                       # AND #$01
Emit @(0xAA)                             # TAX
Emit @(0xBD, 0x68, 0x80)                 # LDA dpcmaddr,X       ($8068)
Emit @(0x8D, 0x12, 0x40)                 # STA $4012 - sample address
Emit @(0xA9, 0x01, 0x8D, 0x13, 0x40)     # LDA #$01 : STA $4013 - 17 bytes
Emit @(0xA9, 0x0C, 0x8D, 0x10, 0x40)     # LDA #$0C : STA $4010 - 8.4 kHz, no loop
Emit @(0xA9, 0x1F, 0x8D, 0x15, 0x40)     # LDA #$1F : STA $4015 - retrigger
Emit @(0x60)                             # RTS

# --- tables -------------------------------------------------------------------------------------
Pad-To 0x60
Emit @(0x00, 0x05, 0x0A, 0x0F, 0x00, 0x05, 0x0A, 0x0F)   # noise periods, four distinct

Pad-To 0x68
Emit @(0x00, 0x01)                       # $4012 values: $C000 and $C040

# --- the two DPCM samples, at $C000 and $C040 -----------------------------------------------------
# Different bytes on purpose: the scan keys a note on the address, FamiStudio keys it on the
# sample's contents, and identical contents would let the two disagree without either noticing.
Pad-To 0x4000
Emit ([byte[]]@(0xAA) * 17)              # 10101010 - a square the DAC can follow
Pad-To 0x4040
Emit ([byte[]]@(0xCC) * 17)              # 11001100 - a wider one, so the two differ

$header = New-Object System.Collections.Generic.List[byte]
$header.AddRange([System.Text.Encoding]::ASCII.GetBytes('NESM'))
$header.Add(0x1A)
$header.Add(1)                           # version
$header.Add(1)                           # songs
$header.Add(1)                           # starting song

function Add-U16([uint16]$v) {
	$header.Add([byte]($v -band 0xFF))
	$header.Add([byte](($v -shr 8) -band 0xFF))
}

Add-U16 0x8000                           # load address
Add-U16 0x8000                           # init address
Add-U16 ([uint16]$playAddress)           # play address

$header.AddRange([System.Text.Encoding]::ASCII.GetBytes('APU percussion'))
while ($header.Count -lt 0x6E) { $header.Add(0) }
Add-U16 16666                            # NTSC speed
while ($header.Count -lt 0x78) { $header.Add(0) }
Add-U16 19997                            # PAL speed
$header.Add(0x00)                        # chip flags: the 2A03 only
while ($header.Count -lt 0x80) { $header.Add(0) }

$bytes = New-Object System.Collections.Generic.List[byte]
$bytes.AddRange($header)
$bytes.AddRange($rom)

$out = Join-Path $PSScriptRoot 'apu_percussion.nsf'
[System.IO.File]::WriteAllBytes($out, $bytes.ToArray())
"wrote {0} ({1} bytes, play at 0x{2:X4})" -f $out, $bytes.Count, $playAddress
