# Builds the Dn-FamiTracker reference implementation, which is what an authoritative comparison needs.
#
# The solution targets toolset v143 and a Windows SDK that this machine does not have at the default
# location: the MSVC toolset here is 14.51 (v145) and the SDK lives on D: because the registry's
# KitsRoot10 says so. Both are passed explicitly rather than by editing the project files, so the
# reference tree stays pristine.
param(
    [string] $Configuration = "Release",
    [string] $Platform = "Win32",
    [string] $ProjectDir = "reference\Dn-FamiTracker"
)

$ErrorActionPreference = "Stop"

$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild))
{
    throw "MSBuild not found at $msbuild"
}

$kitsRoot = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots").KitsRoot10
$includeRoot = Join-Path $kitsRoot "Include"
if (-not (Test-Path $includeRoot))
{
    throw "No Windows SDK under $includeRoot - install the 'Windows SDK' component"
}

$sdk = (Get-ChildItem $includeRoot -Directory | Sort-Object Name | Select-Object -Last 1).Name
Write-Host "toolset v145, SDK $sdk"

Push-Location $ProjectDir
try
{
    & $msbuild "Dn-FamiTracker.sln" /nologo `
        "/p:Configuration=$Configuration" "/p:Platform=$Platform" `
        "/p:PlatformToolset=v145" "/p:WindowsTargetPlatformVersion=$sdk" `
        /p:TreatWarningAsError=false `
        /m /v:m
}
finally
{
    Pop-Location
}
