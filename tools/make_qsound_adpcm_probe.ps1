$out = 'test_file\QSound\99 ADPCM probe.vgm'
$romSize = 256

$d = [System.Collections.Generic.List[byte]]::new()

# data block: 0x67 0x66, type 0x8F (QSound ROM), size as four little-endian bytes
$d.Add(0x67); $d.Add(0x66); $d.Add(0x8F)
$d.Add([byte]($romSize -band 0xFF))
$d.Add([byte](($romSize -shr 8) -band 0xFF))
$d.Add([byte](($romSize -shr 16) -band 0xFF))
$d.Add([byte](($romSize -shr 24) -band 0xFF))

# 0x88 and 0x77 alternate: nibbles of -8 and +7, the widest swings the format has,
# so the decoded waveform is a square-ish wave rather than a slow drift.
for ($i = 0; $i -lt $romSize; $i++) {
    $d.Add($(if ($i % 2 -eq 0) { 0x88 } else { 0x77 }))
}

# ADPCM voice 0: start 0x0010, end 0x0090, bank 0x8000 (sample ROM), volume 0x7FFF.
foreach ($r in @(@(0x0010, 0xCA), @(0x0090, 0xCB), @(0x8000, 0xCC), @(0x7FFF, 0xCD))) {
    $d.Add(0xC4)
    $d.Add([byte](($r[0] -shr 8) -band 0xFF))
    $d.Add([byte]($r[0] -band 0xFF))
    $d.Add([byte]$r[1])
}

# Start it, wait a second, start it again, wait another second.
for ($k = 0; $k -lt 2; $k++) {
    $d.Add(0xC4); $d.Add(0x00); $d.Add(0x01); $d.Add(0xD6)   # flag = 1
    $d.Add(0x61); $d.Add(0x44); $d.Add(0xAC)                 # wait 44100 samples
}

$d.Add(0x66)   # end of stream

$total = 0xA0 + $d.Count
$h = New-Object byte[] 0xA0

$sig = [System.Text.Encoding]::ASCII.GetBytes('Vgm ')
[Array]::Copy($sig, 0, $h, 0, 4)

$h[0x08] = 0x51; $h[0x09] = 0x01     # version 1.51
$h[0x24] = 60                        # rate
$h[0x34] = 0x6C                      # VGM data offset: 0xA0 - 0x34

$clk = [BitConverter]::GetBytes([uint32]40000000)
[Array]::Copy($clk, 0, $h, 0x9C, 4)  # QSound clock

$eof = [BitConverter]::GetBytes([uint32]($total - 4))
[Array]::Copy($eof, 0, $h, 0x04, 4)

$all = New-Object byte[] $total
[Array]::Copy($h, 0, $all, 0, 0xA0)
$d.CopyTo($all, 0xA0)

[System.IO.File]::WriteAllBytes($out, $all)

Write-Output ("  wrote {0}: {1} bytes ({2} header + {3} data)" -f $out, $total, 0xA0, $d.Count)
