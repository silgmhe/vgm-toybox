<#
    Registers VGMToybox as a handler for the chiptune formats it plays - the same thing Explorer's
    "Open with" dialog does, only scriptable and reversible. Everything is written under HKCU, so
    it needs no administrator and touches no other user's settings.

        .\register-file-types.ps1               add VGMToybox to Explorer's "Open with" list
        .\register-file-types.ps1 -SetDefault   ... and make it the default for these formats
        .\register-file-types.ps1 -Unregister   undo it (other handlers are left alone)
        .\register-file-types.ps1 -WhatIf       print what would be written, write nothing

    After registering, a double-click plays the song because the application loads the file it is
    started with and starts the transport (PluginProcessor::getStartupFile()).

    The extension list mirrors the playback backends - LibvgmBackend::getFileExtensions() returns
    ".vgm .vgz .s98 .dro .gym" - so a new format belongs in both places.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    # left empty here on purpose: a default that reads $PSScriptRoot is evaluated before that
    # variable exists, and comes out empty - it is resolved below instead
    [string] $ExePath = '',
    [switch] $SetDefault,
    [switch] $Unregister
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($ExePath))
{
    $here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
    $ExePath = Join-Path $here 'Standalone\VGMToybox.exe'
}

$Extensions = @('vgm', 'vgz', 's98', 'dro', 'gym')
$ProgId = 'VGMToybox.Song'
$Classes = 'HKCU:\Software\Classes'
$AppKey = "$Classes\Applications\VGMToybox.exe"
$ProgIdKey = "$Classes\$ProgId"

function Set-KeyValue([string] $Path, [string] $Name, [string] $Value)
{
    if (-not $PSCmdlet.ShouldProcess($Path, "set $Name")) { return }
    if (-not (Test-Path -LiteralPath $Path)) { New-Item -Path $Path -Force | Out-Null }
    New-ItemProperty -LiteralPath $Path -Name $Name -Value $Value -PropertyType String -Force | Out-Null
}

function Remove-Key([string] $Path)
{
    if (-not $PSCmdlet.ShouldProcess($Path, 'remove')) { return }
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
}

function Get-DefaultValue([string] $Path)
{
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return (Get-ItemProperty -LiteralPath $Path -ErrorAction SilentlyContinue).'(default)'
}

if ($Unregister)
{
    Remove-Key $AppKey
    Remove-Key $ProgIdKey

    foreach ($ext in $Extensions)
    {
        # Only clear a default we set ourselves: an extension pointing at some other player is
        # none of our business, and the key itself stays (it may hold other applications).
        $key = "$Classes\.$ext"
        if ((Get-DefaultValue $key) -eq $ProgId)
        {
            Set-KeyValue $key '(default)' ''
        }
    }

    Write-Host 'VGMToybox was removed from the file type handlers.'
    exit 0
}

# --- the executable has to exist before anything is written -------------------------------------
if (-not (Test-Path -LiteralPath $ExePath))
{
    Write-Host "Cannot find '$ExePath'."
    Write-Host 'Pass -ExePath <path to VGMToybox.exe> if it lives somewhere else.'
    exit 1
}

$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
$Command = '"' + $ExePath + '" "%1"'

# --- Explorer's "Open with" list -----------------------------------------------------------------
Set-KeyValue "$AppKey\shell\open\command" '(default)' $Command
Set-KeyValue $AppKey 'FriendlyAppName' 'VGMToybox'

foreach ($ext in $Extensions)
{
    Set-KeyValue "$AppKey\SupportedTypes" ".$ext" ''
}

# --- the default handler (only when asked) --------------------------------------------------------
if ($SetDefault)
{
    Set-KeyValue $ProgIdKey '(default)' 'VGMToybox song'
    Set-KeyValue "$ProgIdKey\shell\open\command" '(default)' $Command
    Set-KeyValue "$ProgIdKey\DefaultIcon" '(default)' ('"' + $ExePath + '",0')

    foreach ($ext in $Extensions)
    {
        Set-KeyValue "$Classes\.$ext" '(default)' $ProgId
    }
}

Write-Host ''
Write-Host ("VGMToybox now opens: " + ($Extensions -join ', '))
if ($SetDefault)
{
    Write-Host 'It is the default handler for those formats.'
}
else
{
    Write-Host "It is in the 'Open with' list. To make it the default, either tick 'Always use this"
    Write-Host "app' in that dialog or run this script again with -SetDefault."
}
