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
    [switch] $Unregister,
    # read-only: report who Windows would actually hand each format to, and which executable that
    # is - the quickest way to tell "the association never fired" from "it fired a stale copy"
    [switch] $Status,
    # try to drop Explorer's per-user "UserChoice" for these formats so the Classes default
    # applies again. Windows writes that entry when someone picks an application in the Open-with
    # dialog, it overrides everything else - including -SetDefault - and a stale one is the usual
    # reason a double click appears to do nothing at all. Best effort only: Windows protects the
    # key, refuses to delete it and prints a note instead (the Open-with dialog's "Always use this
    # app" is the way that always works).
    [switch] $ClearUserChoice
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
    if (-not (Test-Path -LiteralPath $Path)) { return }
    if (-not $PSCmdlet.ShouldProcess($Path, 'remove')) { return }

    # reg.exe rather than Remove-Item: the registry provider throws on a key that holds only values
    # ("Cannot delete a subkey tree because the subkey does not exist"), and Explorer's UserChoice
    # is protected against tampering - which has to be reportable and survivable, not fatal, since
    # the rest of the registration is still worth doing.
    $null = & reg.exe delete ($Path -replace '^HKCU:\\', 'HKCU\') /f 2>&1

    if ($LASTEXITCODE -ne 0)
    {
        Write-Host ("  could not remove {0}" -f $Path)
        Write-Host "  (Windows protects Explorer's UserChoice; use the Open-with dialog and tick"
        Write-Host "   'Always use this app' to change it)"
    }
}

function Get-DefaultValue([string] $Path)
{
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return (Get-ItemProperty -LiteralPath $Path -ErrorAction SilentlyContinue).'(default)'
}

if ($Status)
{
    # Nothing here writes anything: it reports the effective handler per extension. The one Windows
    # actually uses is the Explorer "UserChoice" when there is one - that entry overrides every
    # Classes default and cannot be written by a script, which is why changing the registry alone
    # sometimes appears to do nothing.
    Write-Host ''
    Write-Host "Effective handlers (this script would register: $ExePath)"
    Write-Host ''

    foreach ($ext in $Extensions)
    {
        $choiceKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.$ext\UserChoice"
        $choice = if (Test-Path -LiteralPath $choiceKey)
                  { (Get-ItemProperty -LiteralPath $choiceKey -Name 'ProgId' -ErrorAction SilentlyContinue).ProgId }
                  else { '' }

        $effective = if ($choice) { $choice } else { Get-DefaultValue "$Classes\.$ext" }
        if (-not $effective) { $effective = Get-DefaultValue "HKLM:\Software\Classes\.$ext" }

        if ($effective)
        {
            # (no inline if in an argument list - that is PowerShell 7 syntax, and this runs on 5.1)
            $note = if ($choice) { '   (by UserChoice - it overrides the Classes defaults)' } else { '' }
            Write-Host ("  .{0,-5} -> {1}{2}" -f $ext, $effective, $note)
        }
        else
        {
            Write-Host ("  .{0,-5} -> (nothing - Windows asks which application to use)" -f $ext)
        }

        if ($effective)
        {
            $command = Get-DefaultValue "$Classes\$effective\shell\open\command"
            if (-not $command) { $command = Get-DefaultValue "HKLM:\Software\Classes\$effective\shell\open\command" }

            if ($command)
            {
                $exe = ($command -replace '^"([^"]+)".*', '$1')
                $exists = Test-Path -LiteralPath $exe
                $stamp = if ($exists) { (Get-Item -LiteralPath $exe).LastWriteTime } else { 'MISSING' }
                Write-Host ("          runs: {0}" -f $command)
                Write-Host ("          file: {0}  [{1}]" -f $stamp, $exe)
            }
            else
            {
                Write-Host '          runs: (no command registered for that ProgId)'
            }
        }
    }

    Write-Host ''
    exit 0
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

# Only now, with a valid executable in hand: dropping Explorer's user choice while the application
# to fall back to cannot be found would leave the formats unassociated altogether.
if ($ClearUserChoice)
{
    foreach ($ext in $Extensions)
    {
        $key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.$ext\UserChoice"
        if (Test-Path -LiteralPath $key)
        {
            Remove-Key $key
            Write-Host "cleared Explorer's user choice for .$ext"
        }
    }
}

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
