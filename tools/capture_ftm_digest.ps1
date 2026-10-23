# Computes the FTM regression digest: per module, how many notes, bends and slides the loader
# produced, plus a hash of the full event dump.
#
# The dump itself is 3.5 MB for seven modules and would drown the repository; the digest is three
# lines and still fails loudly on any change, because the hash covers every field of every event.
# When a line does change, `vgm_chip_test --ftm-dump <file>` prints the detail to diff.
param(
    [string] $TestBinary = "build\plugin\vgm_chip_test.exe",
    [string] $ModuleDir = "test_file\ftm",
    [string] $Out = "plugin\Tests\golden\ftm_digest.txt"
)

$lines = @()
foreach ($file in Get-ChildItem $ModuleDir -Filter *.ftm | Sort-Object Name) {
    $dump = & $TestBinary --ftm-dump $file.FullName 2>&1
    $text = ($dump | Out-String)
    # strip the path line so the digest is machine independent
    $body = ($dump | Where-Object { $_ -notmatch '^module ' }) -join "`n"

    $notes  = ([regex]::Matches($text, '(?m)^note ')).Count
    $bends  = ([regex]::Matches($text, '(?m)^bend ')).Count
    $slides = ([regex]::Matches($text, '(?m)^slide ')).Count

    $sha = [System.Security.Cryptography.SHA256]::Create()
    $hash = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($body))
    $lines += ("{0} notes={1} bends={2} slides={3} hash={4}" -f
        $file.Name, $notes, $bends, $slides, ([System.BitConverter]::ToString($hash) -replace '-', '').Substring(0, 16).ToLower())
}

$lines | Out-File $Out -Encoding utf8
$lines
