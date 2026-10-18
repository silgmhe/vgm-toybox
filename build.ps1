#Requires -Version 5.1
<#
.SYNOPSIS
    Builds VGMPlay with MSVC + Ninja as a self-contained (statically linked) executable.

.DESCRIPTION
    The script performs a complete one-shot build:
      1. sets up the MSVC environment (vswhere + vcvars64/vcvars32)
      2. configures the project with CMake/Ninja:
           - libvgm is built from the vendored source in libs/ (static libraries)
           - zlib is fetched with CPM.cmake and built from source (no zlib DLL)
           - static MSVC runtime (/MT)  -> no VC++ Redistributable required
           - CMAKE_EXPORT_COMPILE_COMMANDS -> compile_commands.json for clangd
      3. builds the VGMPlay executable
      4. copies VGMPlay*.exe + VGMPlay.ini + the docs, the VST3 bundle and the standalone
        application to .\dist
         and writes compile_commands.json to the repository root
      5. prints the DLL dependencies of the executable

    Required tools (must be in PATH or installed):
      - Visual Studio with the "Desktop development with C++" workload
      - CMake and Ninja (the Ninja shipped with CMake is found automatically)
      - git (CPM fetches the zlib source with it)

    The zlib source is downloaded on the first configure. The download repository can be
    chosen with -DLIBVGM_ZLIB_REPOSITORY=<url> (the CMake build probes GitHub and the
    Gitee mirror automatically) and its source is cached in $env:CPM_SOURCE_CACHE.

.PARAMETER Arch
    Target architecture: x64 (default), x86 or arm64. arm64 needs the ARM64 build tools - either
    installed on an arm64 host, or crossed from an x64 host through vcvarsall x64_arm64.

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

.PARAMETER Target
    Extra CMake targets to build after the default ones. The console tests are EXCLUDE_FROM_ALL -
    they exist only when asked for by name - so this is how a local run builds a single test
    without repeating the CMake invocation. -Test adds them by itself.

.PARAMETER Test
    Build the console tests and run them: the three that need nothing (core, ui, app) and the
    mixer gate, which gets the song below. Fails the script when one of them fails. The tests are
    added to the build, so a run cannot test yesterday's binary.

.PARAMETER TestSong
    The .vgm/.vgz the mixer test and the window scripts load. Defaults to
    plugin/Tests/assets/minimal.vgm (431 bytes of generated SN76489 tone, six seconds long) - pass
    a real song to exercise the backends with something bigger.

.PARAMETER Clang
    After building, re-check the plugin sources with clang-cl (/Zs: syntax and type check only, no
    object files, no binaries). A second opinion on code MSVC accepts - see Invoke-ClangCheck. Needs
    the compile_commands.json this script writes, so it runs as part of a normal build. When
    clang-cl is not installed the check is skipped and the build result is unchanged.

.EXAMPLE
    .\build.ps1
    Builds a release build for x64 (command-line player + JUCE plugin).

.EXAMPLE
    .\build.ps1 -Config Debug -Clean -NoPlugin
    Rebuilds everything from scratch with debug settings, without the plugin.

.EXAMPLE
    .\build.ps1 -Test
    The command CI runs: build everything (the console tests included), then run them. None of them
    needs a window or a sound device, and the song they load is the one in the repository.

.EXAMPLE
    .\build.ps1 -Clang
    The same build, and then the plugin sources through clang as well - what the macOS build sees,
    available here before pushing.
#>
[CmdletBinding()]
param(
	[ValidateSet('x64', 'x86', 'arm64')]
	[string]$Arch = 'x64',

	[ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
	[string]$Config = 'Release',

	[switch]$Clean,

	[string]$Proxy = '',

	[switch]$NoProxy,

	[switch]$NoPlugin,

	# Extra CMake targets to build after the default ones (the console tests are EXCLUDE_FROM_ALL).
	# One comma-separated list: PowerShell refuses a parameter that is given twice.
	#     .\build.ps1 -Target vgm_app_test
	[string[]]$Target = @(),

	# Build and run the console tests, and fail the script when one of them fails. This adds the
	# test targets to the build: without them, -Test would run whatever binary happens to be in
	# build/plugin - and a stale one is how v0.1.0 went out on a local "all checks passed"
	# (docs/memo/2026-10-17-stale-test-binary-and-the-restore-test.md).
	[switch]$Test,

	# Song for the mixer test and the window scripts; empty means the one in the repository.
	[string]$TestSong = '',

	# Re-check the plugin sources with clang-cl after building (no output produced). Optional, and
	# skipped without an error when clang is not installed.
	[switch]$Clang
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

	# Native tools write progress and diagnostics to stderr - git's "Cloning into ...", MSVC's
	# banners - and with $ErrorActionPreference = 'Stop' PowerShell turns any of that into a
	# terminating error, which is how a perfectly healthy dependency clone aborted this script.
	# The exit code below is the real answer, so stderr must not be treated as one.
	$previousPreference = $ErrorActionPreference
	$ErrorActionPreference = 'Continue'
	try {
		& $FilePath @Arguments
	} finally {
		$ErrorActionPreference = $previousPreference
	}

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

	# arm64 comes in two shapes: the ARM64 build tools running natively on an arm64 host, or the
	# x64_arm64 cross tools that vcvarsall sets up on an x64 host. vcvars64/vcvars32 cover the rest.
	if ($TargetArch -eq 'x86') {
		$vcvarsName = 'vcvars32.bat'
		$vcvarsArgs = ''
	} elseif ($TargetArch -eq 'arm64') {
		if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') {
			$vcvarsName = 'vcvarsarm64.bat'
			$vcvarsArgs = ''
		} else {
			$vcvarsName = 'vcvarsall.bat'
			$vcvarsArgs = ' x64_arm64'
		}
	} else {
		$vcvarsName = 'vcvars64.bat'
		$vcvarsArgs = ''
	}
	$vcvars = Join-Path $vsPath ("VC\Auxiliary\Build\{0}" -f $vcvarsName)
	if (-not (Test-Path -LiteralPath $vcvars)) {
		throw "'$vcvars' not found - is the C++ workload (and, for -Arch arm64, the ARM64 build tools) installed?"
	}
	Write-Host "Visual Studio: $vsPath" -ForegroundColor DarkGray
	Write-Host "vcvars       : $vcvars$vcvarsArgs" -ForegroundColor DarkGray

	# vcvars*.bat can only be used by cmd.exe - dump its environment and import it.
	New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
	$envBat = Join-Path $BuildDir '_msvc_env.bat'
	@(
		'@echo off'
		"call `"$vcvars`"$vcvarsArgs >nul"
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

function Invoke-ClangUnit([string]$Compiler, [string]$CommandLine, [string]$WorkingDirectory) {
	# One complete command line, handed over through ProcessStartInfo so that it reaches the compiler
	# exactly as CMake wrote it. Passing the arguments as a PowerShell array instead loses the quotes
	# around defines such as
	#     -DJucePlugin_Desc="Chiptune player (VGM, VGZ, S98, DRO, GYM)"
	# - the C runtime eats them on the way - which turns one argument into four files that do not
	# exist, and the check then fails for reasons that have nothing to do with the code.
	$startInfo = New-Object System.Diagnostics.ProcessStartInfo
	# FileName and Arguments are joined into one string before the process is started, so a compiler
	# inside "Program Files" has to carry its quotes here.
	$startInfo.FileName = if ($Compiler -match '\s') { '"' + $Compiler + '"' } else { $Compiler }
	$startInfo.Arguments = $CommandLine
	$startInfo.WorkingDirectory = $WorkingDirectory
	$startInfo.UseShellExecute = $false
	$startInfo.CreateNoWindow = $true
	$startInfo.RedirectStandardOutput = $true
	$startInfo.RedirectStandardError = $true

	$process = [System.Diagnostics.Process]::Start($startInfo)
	$diagnostics = $process.StandardError.ReadToEnd() + $process.StandardOutput.ReadToEnd()
	$process.WaitForExit()

	return [pscustomobject]@{ ExitCode = $process.ExitCode; Diagnostics = $diagnostics }
}

function Invoke-ClangCheck {
	# A second opinion on the C++ the MSVC build just accepted. clang is stricter in places MSVC is
	# not - an empty braced-init-list against juce::File's copy and move assignment is the case that
	# went through a green Windows build and would have stopped the macOS one - so each translation
	# unit's own compile command is replayed through clang-cl with /Zs: parse and type check only,
	# no object files, nothing written to the build tree.
	#
	# clang is optional. This runs after the build, so not having it changes nothing; it is an extra
	# pair of eyes, not a requirement.
	$clang = Get-Command 'clang-cl.exe' -ErrorAction SilentlyContinue
	if (-not $clang) {
		Write-Host 'clang-cl not found - skipping the clang check' -ForegroundColor DarkGray
		return
	}
	if (-not (Test-Path -LiteralPath $CompileDb)) {
		Write-Warning 'no compile_commands.json - skipping the clang check'
		return
	}

	# One entry per file: the same source appears once per target that compiles it, and the flags
	# differ only by target-specific defines.
	$database = Get-Content -LiteralPath $CompileDb -Raw | ConvertFrom-Json
	$units = @{}
	foreach ($entry in $database) {
		if ($entry.file -match '[\\/]plugin[\\/](Source|Tests)[\\/]' -and
			-not $units.ContainsKey($entry.file.ToLowerInvariant())) {
			$units[$entry.file.ToLowerInvariant()] = $entry
		}
	}
	if ($units.Count -eq 0) {
		Write-Warning 'compile_commands.json lists no plugin sources - skipping the clang check'
		return
	}

	$rootPrefix = $RepoRoot.Replace('\', '/') + '/'
	Write-Host ("checking {0} translation unit(s), nothing written" -f $units.Count) -ForegroundColor DarkGray
	$failed = @()

	foreach ($entry in ($units.Values | Sort-Object -Property @{ Expression = { $_.file } })) {
		if (-not $entry.PSObject.Properties['command'] -or -not $entry.command) {
			Write-Warning ("no compile command for '{0}' - skipped" -f $entry.file)
			continue
		}

		# The build's own command line, minus the compiler (Invoke-ClangUnit starts clang itself) and
		# minus the outputs; the quoting is left exactly as CMake wrote it (that function says why).
		$unitCommand = $entry.command -replace '^\s*\S+', ''
		$unitCommand = $unitCommand -replace '\s+/Fo\S+', ''
		$unitCommand = $unitCommand -replace '\s+/Fd\S+', ''
		$unitCommand = $unitCommand -replace '\s+-c(?=\s|$)', ''
		$unitCommand += ' /Zs'

		$relative = $entry.file
		if ($relative.StartsWith($rootPrefix)) { $relative = $relative.Substring($rootPrefix.Length) }
		Write-Host ('  ' + $relative) -ForegroundColor DarkGray

		$result = Invoke-ClangUnit -Compiler $clang.Source -CommandLine $unitCommand -WorkingDirectory $entry.directory

		if ($result.ExitCode -ne 0) {
			$errors = @($result.Diagnostics -split "`r?`n" | Where-Object { $_ -match ': error' } | Select-Object -First 4)
			if ($errors.Count -eq 0) { $errors = @("clang-cl exited with $($result.ExitCode)") }
			$failed += [pscustomobject]@{ File = $relative; Errors = $errors }
		}
	}

	if ($failed.Count -gt 0) {
		foreach ($failure in $failed) {
			Write-Host ('  ' + $failure.File) -ForegroundColor Red
			foreach ($error in $failure.Errors) {
				Write-Host ('    ' + $error) -ForegroundColor Red
			}
		}
		throw ("clang rejects {0} file(s) that MSVC accepted - the macOS build would fail here" -f $failed.Count)
	}
	Write-Host ("  {0} file(s) accepted by clang" -f $units.Count) -ForegroundColor Green
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

Write-Step 'Checking the vendored libraries'
# libvgm, inih and wingetopt are source in this repository (no submodules), so a missing one means
# an incomplete checkout - there is nothing to fetch.
foreach ($library in @('libs\libvgm', 'libs\inih', 'libs\wingetopt')) {
	if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $library))) {
		throw "The vendored library '$library' is missing - it is part of this repository."
	}
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

# The song the console tests load. The repository carries one (431 bytes, generated for this: six
# seconds of SN76489 tone), so a checkout can exercise a real load/render/mix path without a
# download; -TestSong replaces it with something bigger. It goes in as a cache entry so the CTest
# properties and the mixer gate use the same file.
if (-not $TestSong) {
	$TestSong = Join-Path $RepoRoot 'plugin\Tests\assets\minimal.vgm'
}
if (Test-Path -LiteralPath $TestSong) {
	$cmakeOptions += ("-DTOYBOX_TEST_SONG=" + (Resolve-Path -LiteralPath $TestSong).Path.Replace('\', '/'))
} else {
	# Clear the cached value as well: leaving the previous song in the cache would make the tests
	# keep loading a file this run never found.
	$cmakeOptions += '-DTOYBOX_TEST_SONG='
	Write-Warning ("no test song at '{0}' - the tests that need one will skip" -f $TestSong)
}

Invoke-Native 'cmake' (@(
	'-S', $sourceDir,
	'-B', $buildDir,
	'-G', 'Ninja'
) + $cmakeOptions)

Write-Step 'Building'
Invoke-Native 'cmake' @('--build', $buildDir)

# -Test means "test what is in the tree", so the console tests are built here rather than assumed:
# they are EXCLUDE_FROM_ALL, and a -Test that runs a binary nobody just built is exactly the trap
# v0.1.0 fell into (docs/memo/2026-10-17-stale-test-binary-and-the-restore-test.md).
if ($Test) {
	$Target = @($Target + @('vgm_core_test', 'vgm_ui_test', 'vgm_app_test', 'vgm_mixer_test') |
		Select-Object -Unique)
}

if ($Target.Count -gt 0) {
	Write-Step ("Building extra targets: {0}" -f ($Target -join ', '))
	Invoke-Native 'cmake' (@('--build', $buildDir, '--target') + $Target)
}

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
$aaxBundle = $null
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
	$aaxBundle = Get-ChildItem -LiteralPath $artefactsDir -Filter 'VGMToybox.aaxplugin' -Directory -Recurse |
		Select-Object -First 1
	if ($aaxBundle) {
		$aaxTarget = Join-Path $DistDir 'AAX'
		New-Item -ItemType Directory -Force -Path $aaxTarget | Out-Null
		try {
			Copy-Item -LiteralPath $aaxBundle.FullName -Destination $aaxTarget -Recurse -Force -ErrorAction Stop
		} catch {
			throw ("Could not copy the AAX bundle to '{0}' - Pro Tools is probably still using it. " +
					"Close it and run the script again. ({1})" -f $aaxTarget, $_.Exception.Message)
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

if ($Clang) {
	Write-Step 'Checking with clang'
	Invoke-ClangCheck
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
if ($aaxBundle) {
	Write-Host ("AAX        : {0}" -f (Join-Path $DistDir 'AAX\VGMToybox.aaxplugin'))
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

# --- the console tests -------------------------------------------------------------------------
# Ordinary programs: text in, ok/FAIL out, exit code for CI. None of them needs a window - the
# harness constructs the real editor headless (docs/desgin/testing-architecture.md) - and the song
# the ones that want one load is in the repository, so they run on a build agent as they do here.
if ($Test) {
	Write-Step 'Running the console tests'

	# The list lives in plugin/CMakeLists.txt: core, ui, engine and one test per window script.
	# This used to be a hand-written loop here (and a second copy in each CI job); ctest now owns
	# the list, the per-test timeout and the JUnit report.
	$ctest = Get-Command 'ctest' -ErrorAction SilentlyContinue
	if (-not $ctest) {
		throw 'ctest was not found - it ships with CMake, which the build already needs'
	}

	$ctestArgs = @('--test-dir', $BuildDir, '--output-on-failure')
	$ctestVersion = ((& ctest --version | Select-Object -First 1) -replace '^[^0-9]*', '')
	if ($ctestVersion -match '^\d+\.\d+') {
		# --output-junit arrived in CMake 3.21; older ctests still run, just without the report.
		if ([version]$Matches[0] -ge [version]'3.21') {
			$ctestArgs += @('--output-junit', (Join-Path $BuildDir 'test-results.xml'))
		}
	}

	# Invoke-Native keeps ctest's stderr from becoming a terminating error and turns its exit code
	# into the script's; --output-on-failure has already printed which test failed.
	Invoke-Native 'ctest' $ctestArgs

	Write-Host '  all registered tests passed' -ForegroundColor Green
}
