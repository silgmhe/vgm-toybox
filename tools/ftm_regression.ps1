# Local regression for the FTM loader, compared against FamiTracker itself.
#
# The authority is the text export Dn-FamiTracker writes with `-export`; the exports are generated
# once and kept, so a run needs no tracker, only Python and the test binary. CI runs the unit tests
# (ctest); this script is the full comparison a release should pass.
#
#   tools/ftm_regression.ps1                 # compare every module with an export
#   tools/ftm_regression.ps1 -Export         # regenerate the exports first (needs the tracker)
# 注意：reference/ 是本地参考树并被 .gitignore 排除；构建、ctest 与自动化流程都不依赖它，
#       只有本脚本需要（且缺树时应自行失败，而不是静默产出错误结果）。
param(
    [string] $ModuleDir = "test_file\ftm",
    [string] $ExportDir = "build\ftm_exports",
    [string] $TestBinary = "build\plugin\vgm_chip_test.exe",
    [string] $TrackerExe = "reference\Dn-FamiTracker\Win32\Release\Dn-FamiTracker.exe",
    [switch] $Export
)

$ErrorActionPreference = "Stop"
$failures = 0
$compared = 0

if ($Export)
{
    if (-not (Test-Path $TrackerExe)) { throw "Tracker not built: $TrackerExe" }
    New-Item -ItemType Directory -Force $ExportDir | Out-Null
    foreach ($module in Get-ChildItem $ModuleDir -Filter *.ftm)
    {
        $out = Join-Path (Resolve-Path $ExportDir) ($module.BaseName + ".txt")
        Start-Process -FilePath (Resolve-Path $TrackerExe) -Wait -NoNewWindow `
            -ArgumentList "`"$($module.FullName)`"", "-export", "`"$out`"" | Out-Null
        Write-Host ("exported " + $module.Name)
    }
}

foreach ($module in Get-ChildItem $ModuleDir -Filter *.ftm | Sort-Object Name)
{
    $text = Join-Path $ExportDir ($module.BaseName + ".txt")
    if (-not (Test-Path $text))
    {
        Write-Host ("SKIP  " + $module.Name + "  (no export)")
        continue
    }

    python tools/ftm_from_text.py $text |
        Where-Object { $_ -match '^note ' } |
        Out-File build\ftm_ref.txt -Encoding utf8

    & $TestBinary --ftm-dump $module.FullName |
        Select-String -Pattern "^note " |
        Where-Object { $_.Line -match 'track=[012] ' } |
        ForEach-Object { $_.Line } |
        Out-File build\ftm_ours.txt -Encoding utf8

    $result = python -c @"
import re
def load(path):
    out = []
    for line in open(path, encoding='utf-8'):
        m = re.match(r'note track=(\d+)(?: channel=-?\d+ type=-?\d+)? pitch=(\d+) start=(\d+) end=(\d+)', line)
        if m: out.append(tuple(int(x) for x in m.groups()))
    return sorted(out)
a = load('build/ftm_ref.txt'); b = load('build/ftm_ours.txt')
bad = sum(1 for x, y in zip(a, b) if x != y) + abs(len(a) - len(b))
print('%d %d %d' % (len(a), len(b), bad))
"@

    $compared++
    $parts = $result -split '\s+'
    $ref = [int] $parts[0]
    $ours = [int] $parts[1]
    $bad = [int] $parts[2]

    if ($bad -eq 0)
    {
        Write-Host ("PASS  " + $module.Name + "  " + $ours + " notes")
    }
    else
    {
        $failures++
        Write-Host ("FAIL  " + $module.Name + "  tracker=" + $ref + " ours=" + $ours + " differences=" + $bad)
    }
}

Write-Host ""
Write-Host ("FTM regression: " + ($compared - $failures) + "/" + $compared + " modules match FamiTracker")
exit $(if ($failures -eq 0) { 0 } else { 1 })
