#Requires -Version 5.1
<#
.SYNOPSIS
    Builds VGMPlay with MSVC + Ninja as a self-contained (statically linked) executable.

.DESCRIPTION
    The script performs a complete one-shot build:
      1. sets up the MSVC environment (vswhere + vcvars64/vcvars32)
      2. configures the project with CMake/Ninja:
           - libvgm is built from the in-tree submodule (static libraries)
           - zlib is fetched with CPM.cmake and built from source (no zlib DLL)
           - static MSVC runtime (/MT)  -> no VC++ Redistributable required
           - CMAKE_EXPORT_COMPILE_COMMANDS -> compile_commands.json for clangd
      3. builds the VGMPlay executable
      4. copies VGMPlay*.exe + VGMPlay.ini + the docs to .\dist
         and writes compile_commands.json to the repository root
      5. prints the DLL dependencies of the executable

    Required tools (must be in PATH or installed):
      - Visual Studio with the "Desktop development with C++" workload
      - CMake and Ninja (the Ninja shipped with CMake is found automatically)
      - git (only to initialize the libvgm submodule)

    The zlib source is downloaded on the first configure. The download repository can be
    chosen with -DLIBVGM_ZLIB_REPOSITORY=<url> (the CMake build probes GitHub and the
    Gitee mirror automatically) and its source is cached in $env:CPM_SOURCE_CACHE.

.PARAMETER Arch
    Target architecture: x64 (default) or x86.

.PARAMETER Config
    CMake build configuration: Release (default), Debug, RelWithDebInfo or MinSizeRel.

.PARAMETER Clean
    Delete the build/dist directories and compile_commands.json before configuring.

.PARAMETER Proxy
    Proxy URL used for the dependency downloads (e.g. http://127.0.0.1:7897). Defaults to the
    Windows system proxy. Relevant because git/CPM do not read the Windows proxy settings.

.PARAMETER NoProxy
    Do not use any proxy for the dependency downloads.

.PARAMETER NoPlugin
    Only build the command-line player and skip the JUCE plugin (much faster).

.EXAMPLE
    .\build.ps1
    Builds a release build for x64 (command-line player + JUCE plugin).

.EXAMPLE
    .\build.ps1 -Config Debug -Clean -NoPlugin
    Rebuilds everything from scratch with debug settings, without the plugin.
#>
[CmdletBinding()]
param(
	[ValidateSet('x64', 'x86')]
	[string]$Arch = 'x64',

	[ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
	[string]$Config = 'Release',

	[switch]$Clean,

	[string]$Proxy = '',

	[switch]$NoProxy,

	[switch]$NoPlugin
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = $PSScriptRoot
$BuildDir = Join-Path $RepoRoot 'build'
$DistDir = Join-Path $RepoRoot 'dist'
$CompileDb = Join-Path $RepoRoot 'compile_commands.json'

# --- static C/C++ runtime: "MultiThreaded" == /MT, "MultiThreadedDebug" == /MTd ---
if ($Config -eq 'Debug') { $RuntimeLib = 'MultiThreadedDebug' } else { $RuntimeLib = 'MultiThreaded' }

function Write-Step([string]$Message) {
	Write-Host ''
	Write-Host "=== $Message ===" -ForegroundColor Cyan
}

function Invoke-Native([string]$FilePath, [string[]]$Arguments) {
	Write-Host ("+ {0} {1}" -f $FilePath, ($Arguments -join ' ')) -ForegroundColor DarkGray
	& $FilePath @Arguments
	if ($LASTEXITCODE -ne 0) {
		throw "'$FilePath' failed with exit code $LASTEXITCODE"
	}
}

function Initialize-MsvcEnvironment {
	param([Parameter(Mandatory)][string]$TargetArch)

	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (-not (Test-Path -LiteralPath $vswhere)) {
		throw "vswhere.exe not found - please install Visual Studio 2019/2022/2026 with the C++ workload."
	}
	$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
	if (-not $vsPath) {
		$vsPath = & $vswhere -latest -products * -property installationPath
	}
	if (-not $vsPath) {
		throw "No Visual Studio installation with C++ tools found."
	}

	if ($TargetArch -eq 'x86') { $vcvarsName = 'vcvars32.bat' } else { $vcvarsName = 'vcvars64.bat' }
	$vcvars = Join-Path $vsPath ("VC\Auxiliary\Build\{0}" -f $vcvarsName)
	if (-not (Test-Path -LiteralPath $vcvars)) {
		throw "'$vcvars' not found - is the C++ workload installed?"
	}
	Write-Host "Visual Studio: $vsPath" -ForegroundColor DarkGray
	Write-Host "vcvars       : $vcvars" -ForegroundColor DarkGray

	# vcvars*.bat can only be used by cmd.exe - dump its environment and import it.
	New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
	$envBat = Join-Path $BuildDir '_msvc_env.bat'
	@(
		'@echo off'
		"call `"$vcvars`" >nul"
		'if errorlevel 1 exit /b 1'
		'set'
	) | Set-Content -LiteralPath $envBat -Encoding ASCII

	$envLines = & cmd.exe /c "`"$envBat`""
	if ($LASTEXITCODE -ne 0) {
		throw "Failed to initialize the MSVC environment via '$vcvars'."
	}
	foreach ($line in $envLines) {
		if ($line -match '^([^=]+)=(.*)$') {
			[System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
		}
	}
	if (-not (Get-Command 'cl.exe' -ErrorAction SilentlyContinue)) {
		throw "cl.exe is not available after initializing the MSVC environment."
	}
}

# Dependency downloads (zlib, JUCE) use git, which does not read the Windows proxy settings -
# export them as environment variables for this process (and thus for CMake/CPM).
function Initialize-NetworkProxy {
	param([string]$ProxyUrl, [switch]$Disabled)

	if ($Disabled) {
		Write-Host 'proxy usage disabled by -NoProxy' -ForegroundColor DarkGray
		return
	}
	if (-not $ProxyUrl) {
		$settings = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' -ErrorAction SilentlyContinue
		if ($settings -and $settings.ProxyEnable -eq 1 -and $settings.ProxyServer) {
			$server = $settings.ProxyServer
			if ($server -match '=') {
				# "http=host:port;https=host:port"
				$parts = @{}
				foreach ($entry in $server.Split(';')) {
					$pair = $entry.Split('=', 2)
					if ($pair.Count -eq 2) {
						$parts[$pair[0].Trim().ToLowerInvariant()] = $pair[1].Trim()
					}
				}
				if ($parts.ContainsKey('https')) {
					$server = $parts['https']
				} elseif ($parts.ContainsKey('http')) {
					$server = $parts['http']
				} else {
					$server = ''
				}
			}
			if ($server) {
				if ($server -notmatch '^[a-zA-Z][a-zA-Z0-9+.-]*://') {
					$server = "http://$server"
				}
				$ProxyUrl = $server
			}
		}
	}
	if ($ProxyUrl) {
		Write-Host "using proxy $ProxyUrl for dependency downloads" -ForegroundColor DarkGray
		foreach ($name in @('HTTP_PROXY', 'HTTPS_PROXY', 'http_proxy', 'https_proxy', 'ALL_PROXY', 'all_proxy')) {
			[System.Environment]::SetEnvironmentVariable($name, $ProxyUrl, 'Process')
		}
	}
}

if ($Clean) {
	Write-Step 'Cleaning'
	foreach ($dir in @($BuildDir, $DistDir)) {
		if (Test-Path -LiteralPath $dir) {
			Remove-Item -LiteralPath $dir -Recurse -Force
			Write-Host "removed $dir"
		}
	}
	if (Test-Path -LiteralPath $CompileDb) {
		Remove-Item -LiteralPath $CompileDb -Force
		Write-Host "removed $CompileDb"
	}
}

Write-Step 'Initializing the MSVC environment'
Initialize-MsvcEnvironment -TargetArch $Arch

Write-Step 'Setting up dependency downloads'
Initialize-NetworkProxy -ProxyUrl $Proxy -Disabled:$NoProxy

Write-Step 'Checking git submodules'
if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot 'libvgm\CMakeLists.txt'))) {
	Invoke-Native 'git' @('-C', $RepoRoot, 'submodule', 'update', '--init', '--recursive')
}

Write-Step "Configuring ($Arch, $Config, static runtime $RuntimeLib)"
$sourceDir = $RepoRoot.Replace('\', '/')
$buildDir = $BuildDir.Replace('\', '/')
$distDir = $DistDir.Replace('\', '/')
$cmakeOptions = @(
	"-DCMAKE_BUILD_TYPE=$Config",
	"-DCMAKE_MSVC_RUNTIME_LIBRARY=$RuntimeLib",
	"-DCMAKE_INSTALL_PREFIX=$distDir",
	'-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
)
if ($NoPlugin) {
	$cmakeOptions += '-DTOYBOX_BUILD_PLUGIN=OFF'
	Write-Host 'plugin build disabled by -NoPlugin' -ForegroundColor DarkGray
}
# --- the playback core must stay free of format engine headers -------------------------------
# The whole point of the playback port (docs/playback-backend-design.md) is that everything
# outside Source/backends/ is format agnostic - a stray include would silently put libvgm back
# into the UI, so it fails the build instead.
Write-Step 'Checking the backend isolation'
$isolationOffenders = Get-ChildItem -Path (Join-Path $SourceDir 'plugin\Source') -Recurse -File -Include '*.h', '*.cpp' |
	Where-Object { $_.FullName -notmatch '\\backends\\' } |
	Select-String -Pattern '#include\s+"(stdtype\.h|player/|emu/|utils/|vgm/)'
if ($isolationOffenders) {
	$isolationOffenders | ForEach-Object {
		Write-Host ('  {0}:{1}: {2}' -f $_.Path, $_.LineNumber, $_.Line.Trim()) -ForegroundColor Red
	}
	throw 'A file outside plugin/Source/backends/ includes a format engine header.'
}

Invoke-Native 'cmake' (@(
	'-S', $sourceDir,
	'-B', $buildDir,
	'-G', 'Ninja'
) + $cmakeOptions)

Write-Step 'Building'
Invoke-Native 'cmake' @('--build', $buildDir)

Write-Step 'Creating the distribution folder'
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
# The player is a subdirectory project (vgmplayer/), so its executable sits one level down in the
# build tree - search recursively so this keeps working wherever the target lands.
$exe = Get-ChildItem -LiteralPath $BuildDir -Filter 'VGMPlay*.exe' -File -Recurse |
	Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $exe) {
	$exe = Get-ChildItem -LiteralPath $BuildDir -Filter 'vgmplay*' -File -Recurse |
		Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
if (-not $exe) {
	throw "No VGMPlay executable found in '$BuildDir'."
}
Copy-Item -LiteralPath $exe.FullName -Destination $DistDir -Force
# the player's INI template and its two text documents live next to its sources now
foreach ($file in @('VGMPlay.ini', 'Updates.txt', 'VGMPlay.txt')) {
	$src = Join-Path $RepoRoot ('vgmplayer\' + $file)
	if (Test-Path -LiteralPath $src) {
		Copy-Item -LiteralPath $src -Destination $DistDir -Force
	}
}

# the JUCE plugin artefacts (VST3 bundle + standalone application)
$artefactsDir = Join-Path $BuildDir 'plugin\VGMToybox_artefacts'
$vst3Bundle = $null
$standaloneApp = $null
if (Test-Path -LiteralPath $artefactsDir) {
	$vst3Bundle = Get-ChildItem -LiteralPath $artefactsDir -Filter 'VGMToybox.vst3' -Directory -Recurse |
		Select-Object -First 1
	if ($vst3Bundle) {
		$vst3Target = Join-Path $DistDir 'VST3'
		New-Item -ItemType Directory -Force -Path $vst3Target | Out-Null
		try {
			Copy-Item -LiteralPath $vst3Bundle.FullName -Destination $vst3Target -Recurse -Force -ErrorAction Stop
		} catch {
			throw ("Could not copy the VST3 bundle to '{0}' - a host (DAW) is probably still using it. " +
					"Close it and run the script again. ({1})" -f $vst3Target, $_.Exception.Message)
		}
	}
	$standaloneApp = Get-ChildItem -LiteralPath $artefactsDir -Filter 'VGMToybox.exe' -File -Recurse |
		Where-Object { $_.DirectoryName -notlike '*VST3*' } | Select-Object -First 1
	if ($standaloneApp) {
		$standaloneTarget = Join-Path $DistDir 'Standalone'
		New-Item -ItemType Directory -Force -Path $standaloneTarget | Out-Null
		try {
			Copy-Item -LiteralPath $standaloneApp.FullName -Destination $standaloneTarget -Force -ErrorAction Stop
		} catch {
			throw ("Could not copy the standalone application to '{0}' - VGMToybox.exe is probably still running. " +
					"Close it and run the script again. ({1})" -f $standaloneTarget, $_.Exception.Message)
		}
	}
}

Write-Step 'Updating compile_commands.json'
$generatedDb = Join-Path $BuildDir 'compile_commands.json'
if (Test-Path -LiteralPath $generatedDb) {
	Copy-Item -LiteralPath $generatedDb -Destination $CompileDb -Force
	Write-Host "wrote $CompileDb (all sources: vgmplayer, plugin, libvgm, for clangd)"
} else {
	Write-Warning "'$generatedDb' not found - clangd will not have a compilation database."
}

Write-Step 'Result'
$exePath = Join-Path $DistDir $exe.Name
Write-Host ("executable : {0} ({1:N0} bytes)" -f $exePath, (Get-Item -LiteralPath $exePath).Length)
if ($vst3Bundle) {
	Write-Host ("VST3       : {0}" -f (Join-Path $DistDir 'VST3\VGMToybox.vst3'))
}
if ($standaloneApp) {
	Write-Host ("standalone : {0}" -f (Join-Path $DistDir 'Standalone\VGMToybox.exe'))
}
Write-Host ("dist folder: {0}" -f $DistDir)
Write-Host ''

$dumpbin = Get-Command 'dumpbin.exe' -ErrorAction SilentlyContinue
if ($dumpbin) {
	Write-Host 'DLL dependencies:' -ForegroundColor Cyan
	$dependencies = & dumpbin /nologo /dependents $exePath |
		ForEach-Object { if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1] } }
	foreach ($dll in $dependencies) {
		Write-Host "  $dll"
	}
	$badDeps = $dependencies | Where-Object { $_ -match '^(VCRUNTIME|MSVCP|api-ms-win|ucrt|zlib|libiconv)' }
	if ($badDeps) {
		Write-Warning ("unexpected non-system dependencies: {0}" -f ($badDeps -join ', '))
	} else {
		Write-Host 'no VC++ runtime / zlib / iconv dependency - only Windows system DLLs' -ForegroundColor Green
	}
}
