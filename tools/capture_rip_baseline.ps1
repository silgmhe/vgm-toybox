<#
.SYNOPSIS
	Records what every rip in a set of directories says about itself, so a refactor can be checked
	against it afterwards.

.DESCRIPTION
	Runs vgm_mixer_test over each *.vgz and writes one line per file:

		<dir>/<nn>: notes=<sum> PASS|FAIL

	The whole corpus takes about three minutes, which is too long for one command in the tooling, so
	the caller passes a subset with -Only and runs it in chunks. -Append keeps the previous chunks.

	The note count is the number that matters for the MIDI side: it is what the decoders produced.
	PASS/FAIL covers the engine-level checks too, which is how a mixer regression would show up.

.EXAMPLE
	tools/capture_rip_baseline.ps1 -Clear
	tools/capture_rip_baseline.ps1 -Only C219,MSM5232,K007232 -Append
	tools/capture_rip_baseline.ps1 -Only K005289 -Append
#>
param
(
	[string] $Root = 'test_file',
	[string] $Out = 'build/baseline_rips.txt',
	[string] $Only = '',
	[int] $Seconds = 6,
	[int] $SampleRate = 44100,
	[switch] $Append,
	[switch] $Clear
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build\plugin\vgm_mixer_test.exe'
if (-not (Test-Path $exe))
{
	throw "missing $exe - build vgm_mixer_test first"
}

$pattern = '(\d+) note\(s\)'
$lines = New-Object System.Collections.Generic.List[string]

$dirs = if ($Only.Length -gt 0) { $Only -split ',' } else { Get-ChildItem (Join-Path $repo $Root) -Directory | ForEach-Object { $_.Name } }
foreach ($dir in $dirs)
{
	$path = Join-Path $repo (Join-Path $Root $dir)
	if (-not (Test-Path $path))
	{
		Write-Warning "no such directory: $path"
		continue
	}

	foreach ($file in (Get-ChildItem $path -Filter *.vgz | Sort-Object Name))
	{
		$output = & $exe $file.FullName $Seconds $SampleRate 2>&1 | Out-String
		$notes = 0
		foreach ($found in [regex]::Matches($output, $pattern))
		{
			$notes += [int] $found.Groups[1].Value
		}

		$verdict = if ($output -match 'all checks passed') { 'PASS' } else { 'FAIL' }
		$number = $file.Name.Substring(0, 2)
		$lines.Add(('{0}/{1}: notes={2} {3}' -f $dir, $number, $notes, $verdict))
	}
}

$target = Join-Path $repo $Out
$result = New-Object System.Collections.Generic.List[string]
if ($Append -and (Test-Path $target))
{
	foreach ($existing in (Get-Content $target -Encoding utf8))
	{
		if ($existing.Trim().Length -gt 0) { $result.Add($existing) }
	}
}

foreach ($line in $lines) { $result.Add($line) }

$result | Out-File -FilePath $target -Encoding utf8
'{0} file(s) recorded in {1}' -f $lines.Count, $Out
