#### VGMToybox JUCE plugin (VST3 + Standalone) ####
# Builds the GUI/plugin front-end of VGMToybox. The audio rendering is done by the same libvgm
# libraries that the command-line player uses (vgm-player/vgm-emu/vgm-utils, no audio drivers).

# --- JUCE (fetched with CPM) ---
set(JUCE_TAG "8.0.9" CACHE STRING "JUCE version to use")
set(JUCE_MIRRORS "https://github.com/juce-framework/JUCE.git;https://gitee.com/mirrors/JUCE.git"
	CACHE STRING "git repositories to probe for JUCE (in order of preference)")
set(JUCE_REPOSITORY "" CACHE STRING "git repository to fetch JUCE from (empty: probe JUCE_MIRRORS)")

if(NOT TARGET juce_audio_processors)
	# CPM lives next to libvgm; keep the path relative to this directory, so that IDEs can
	# resolve it without knowing the layout of the build tree
	set(_cpm_module "${CMAKE_CURRENT_SOURCE_DIR}/../libs/libvgm/cmake/CPM.cmake")
	if(NOT EXISTS "${_cpm_module}")
		message(FATAL_ERROR "VGM Toybox: '${_cpm_module}' not found - "
			"libvgm is vendored in libs/libvgm/, so this checkout is incomplete")
	endif()

	# pick the first reachable repository (GitHub is not accessible from everywhere)
	if(NOT JUCE_REPOSITORY)
		foreach(_juce_repo IN LISTS JUCE_MIRRORS)
			message(STATUS "VGM Toybox: probing JUCE repository ${_juce_repo} ...")
			file(DOWNLOAD "${_juce_repo}/info/refs?service=git-upload-pack"
				"${CMAKE_CURRENT_BINARY_DIR}/juce_repo_probe.txt" TIMEOUT 6 STATUS _juce_probe_status)
			list(GET _juce_probe_status 0 _juce_probe_code)
			if(_juce_probe_code EQUAL 0)
				set(JUCE_REPOSITORY "${_juce_repo}" CACHE STRING "git repository to fetch JUCE from" FORCE)
				break()
			endif()
		endforeach()
		file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/juce_repo_probe.txt")
	endif()
	if(NOT JUCE_REPOSITORY)
		message(FATAL_ERROR "VGM Toybox: no reachable JUCE repository - "
			"set JUCE_REPOSITORY=<url> or put the JUCE sources into CPM_SOURCE_CACHE")
	endif()

	include("${_cpm_module}")
	CPMAddPackage(
		NAME JUCE
		GIT_REPOSITORY "${JUCE_REPOSITORY}"
		GIT_TAG "${JUCE_TAG}"
		GIT_SHALLOW TRUE
	)
endif()

# The tray menu has a Cocoa implementation on macOS. CMake's default project languages do not
# include Objective-C++; JUCE's modules contain .mm files, but enabling the language here makes
# our own TrayIcon_mac.mm explicit and keeps the source selection below valid.
if(APPLE)
	enable_language(OBJCXX)
endif()

# --- output buses ---------------------------------------------------------------------------
# A VST3 host fixes the bus layout when it loads the plugin, so the number of stereo output
# buses is a build-time constant (Source/playback/OutputRouting.h). Bus 0 is "Out 1/2" - the
# main bus, activated by default - the other buses are inactive until the user enables them in
# the host (the usual multi-output instrument model). Lower it for a lighter plugin:
#   cmake -S . -B build -DTOYBOX_OUTPUT_BUSES=16
set(TOYBOX_OUTPUT_BUSES "64" CACHE STRING
	"Number of stereo output buses the plugin declares (bus 0 = \"Out 1/2\" = the main bus)")

# --- the build's identity, shown in Help > About -------------------------------------------------
# Captured when the project is configured; build.ps1 configures on every run, so a rebuild picks up
# a new commit. A source tree without git (a tarball, say) simply says "unknown", and uncommitted
# changes are marked - an About box naming the wrong build is worse than one that admits it.
set(TOYBOX_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
	execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=8 HEAD
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE _toybox_commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
	execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE _toybox_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

	if(_toybox_commit)
		set(TOYBOX_GIT_COMMIT "${_toybox_commit}")
		if(_toybox_dirty)
			set(TOYBOX_GIT_COMMIT "${_toybox_commit}-dirty")
		endif()
	endif()
endif()
message(STATUS "VGMToybox build commit: ${TOYBOX_GIT_COMMIT}")

# --- Audio Unit (macOS only) -----------------------------------------------------------------------
# JUCE skips formats that do not apply to the platform, but saying it here is clearer than relying on
# that - and the release workflow builds this on macOS alone. No AU_MAIN_TYPE is needed: IS_SYNTH
# below already makes it an instrument.
if(APPLE)
	set(TOYBOX_AU_FORMAT AU)
else()
	set(TOYBOX_AU_FORMAT "")
endif()

# --- AAX (Pro Tools; Windows and macOS) -------------------------------------------------------------
# JUCE 8 carries the AAX SDK inside the module itself (juce_audio_plugin_client/AAX/SDK, and the
# LICENSE.txt next to it offers that code under the GPL v3 as well as under Avid's own terms), so
# nothing has to be downloaded or licensed first to *build* this format. On Linux there is nothing
# to guard either: that SDK path function returns early there and the target's active format list
# drops AAX instead of failing, which is why - unlike VST - this needs no sdk_set call to stay
# valid in a list that every platform configures.
#
# What it cannot do for us is the part that decides whether Pro Tools will load the result: retail
# and beta Pro Tools only load a plugin whose binary PACE has signed (the process starts with an
# Avid approval, PACE supplies the tools and the certificate lives on an iLok). An unsigned
# .aaxplugin loads in Pro Tools *developer* builds and nowhere else, so this stays a developer
# artefact until someone takes that route - which is why the release workflow does not ship it.
#
# AAX is what Pro Tools loads, and JUCE keeps that format only on macOS (any architecture) and on
# Windows x86_64: its own platform filter drops AAX on i386 and on arm64 without a word, so such a
# build simply comes out without it. Asking for it only where it survives lets FORMATS keep naming
# it, while the architectures that cannot carry it get a line in the configure log instead of
# looking like a build that has AAX because nothing was printed.
option(TOYBOX_BUILD_AAX "Build the Pro Tools AAX format (macOS, and Windows x64; loads in Pro Tools developer builds, retail needs PACE signing)" ON)

set(TOYBOX_AAX_FORMAT "")
if(TOYBOX_BUILD_AAX)
	if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
		message(STATUS "TOYBOX_BUILD_AAX is on, but a 32-bit build cannot carry AAX - leaving it out")
	elseif(WIN32 AND (MSVC_CXX_ARCHITECTURE_ID MATCHES "ARM" OR CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM"))
		message(STATUS "TOYBOX_BUILD_AAX is on, but Windows arm64 is not an AAX host - leaving it out")
	else()
		set(TOYBOX_AAX_FORMAT AAX)
	endif()
endif()

# --- plugin target ---
juce_add_plugin(VGMToybox
	# --- identity ---------------------------------------------------------------------------
	# The product is VGMToybox - no spaces anywhere, because the name lands in file names, window
	# titles and command lines. The vendor and author fields are the GitHub account this fork
	# lives under, so a host shows who built the plugin next to the product name (JUCE turns them
	# into JucePlugin_Manufacturer / _ManufacturerWebsite / _ManufacturerEmail and shows the
	# copyright in the platform metadata).
	PRODUCT_NAME "VGMToybox"
	COMPANY_NAME "silgmhe"
	COMPANY_WEBSITE "https://github.com/silgmhe/vgm-toybox"
	COMPANY_EMAIL "silgmhe@gmail.com"
	COMPANY_COPYRIGHT "Copyright (c) 2026 silgmhe - AGPL-3.0-or-later - based on VGMPlay and libvgm by Valley Bell and contributors"
	# The plugin codes are the identity a host keeps for the plugin, so they are deliberately
	# left alone: changing them makes every DAW project that references this plugin lose it (a
	# rename would also let the new build sit next to an installed older one).
	PLUGIN_MANUFACTURER_CODE Vgmp
	PLUGIN_CODE Vgm1
	# Spelled out rather than derived from "com.<COMPANY_NAME>.<target>": the id stays a stable
	# lowercase slug, and it never picks up a space if the names above ever gain one (JUCE warns
	# about those, and build.ps1 treats a cmake warning as a failure).
	BUNDLE_ID "com.vgmtoybox.VGMToybox"
	# the format list is the backend registry's job (it drives the file dialogs and hints), this
	# is just the one-line blurb a host shows
	DESCRIPTION "Chiptune player (VGM, VGZ, S98, DRO, GYM)"
	# juceaide turns these two into the .ico the executable carries (16/32/48/256) and picks the
	# best source per size, so the small one is the artwork drawn for 16 px rather than a
	# downscale of the big one. Both must be ordinary images - an .ico input fails the configure
	# step. assets/icon/make_icon.py is the source of truth for all of them.
	ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/vgmtoybox-1024.png"
	ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/vgmtoybox-16.png"
	FORMATS VST3 Standalone ${TOYBOX_AU_FORMAT} ${TOYBOX_AAX_FORMAT}
	IS_SYNTH TRUE
	# MIDI input is declared although nothing reads it: JUCE registers the VST3 event input bus
	# from this flag at compile time, and an *instrument* class without one is a shape some hosts
	# refuse to load (Ableton Live does not load such a plugin at all). The events are accepted
	# and dropped in processBlock() - the song drives playback, not notes.
	NEEDS_MIDI_INPUT TRUE
	# An event output bus is declared although nothing is emitted yet: it is reserved for a future
	# MIDI-out feature, and having the class advertise it now keeps the routing a host sees stable.
	# processBlock() clears the MIDI buffer, which is also what stops the input events from being
	# echoed back out now that the VST3 wrapper would forward whatever is left in it.
	NEEDS_MIDI_OUTPUT TRUE
	IS_MIDI_EFFECT FALSE
	EDITOR_WANTS_KEYBOARD_FOCUS TRUE
	COPY_PLUGIN_AFTER_BUILD FALSE
	VST3_CATEGORIES "Instrument"
	# macOS: lets the Finder offer this .app under "Open With" (Source/Standalone/Info.plist says
	# why that matters here). It is merged into every bundle format, since the argument is
	# per-plugin; only the standalone is launched by the Finder, so the others never see it used.
	PLIST_TO_MERGE "${CMAKE_CURRENT_SOURCE_DIR}/Source/Standalone/Info.plist"
)

target_sources(VGMToybox PRIVATE
	Source/PluginProcessor.cpp
	Source/PluginEditor.cpp
	Source/playback/BackendRegistry.cpp
	Source/playback/PlaybackEngine.cpp
	Source/playback/DawSync.cpp
	Source/playback/HostClockSource.cpp
	Source/playback/SessionState.cpp
	Source/chips/GbcApuDecoder.cpp
	Source/chips/HuC6280Decoder.cpp
	Source/chips/K051649Decoder.cpp
	Source/chips/NesApuDecoder.cpp
	Source/chips/NoteExtractor.cpp
	Source/chips/PokeyDecoder.cpp
	Source/chips/Saa1099Decoder.cpp
	Source/chips/WswanDecoder.cpp
	Source/chips/OplDecoder.cpp
	Source/chips/Ay8910Decoder.cpp
	Source/chips/Rf5c68Decoder.cpp
	Source/chips/X1_010Decoder.cpp
	Source/chips/C140Decoder.cpp
	Source/chips/C352Decoder.cpp
	Source/chips/K053260Decoder.cpp
	Source/chips/K054539Decoder.cpp
	Source/chips/QSoundDecoder.cpp
	Source/chips/Es5503Decoder.cpp
	Source/chips/K005289Decoder.cpp
	Source/chips/Msm5232Decoder.cpp
	Source/chips/MikeyDecoder.cpp
	Source/chips/MultiPcmDecoder.cpp
	Source/chips/K007232Decoder.cpp
	Source/chips/VsuDecoder.cpp
	Source/chips/Okim6295Decoder.cpp
	Source/chips/Ga20Decoder.cpp
	Source/chips/SegaPcmDecoder.cpp
	Source/chips/Ymf271Decoder.cpp
	Source/chips/Ymz280bDecoder.cpp
	Source/chips/Sn76489Decoder.cpp
	Source/chips/OpmDecoder.cpp
	Source/chips/OpnDecoder.cpp
	Source/backends/BuiltInBackends.cpp
	Source/backends/libvgm/LibvgmBackend.cpp
	Source/backends/libvgm/MidiSequenceBuilder.cpp
	Source/backends/libvgm/VgmCommandReader.cpp
	Source/formats/vgm/VgmSongLoader.cpp
	Source/formats/ftm/FtmSongLoader.cpp
	Source/formats/ftm/FtmSongIr.cpp
	Source/formats/nsf/NsfFile.cpp
	Source/formats/nsf/NsfMemory.cpp
	Source/backends/libvgm/LibvgmSongInstance.cpp
	Source/backends/libvgm/LibvgmTrackPlayer.cpp
	Source/backends/libvgm/LibvgmVgz.cpp

	# The NES backend: our adapter and the NSFPlay cores it drives. The cores are vendored under
	# Source/backends/nes/third_party and are compiled into the plugin itself - see
	# docs/review/2026-10-24-nes-backend-plan.md for why this exists at all (libvgm has no VRC6).
	Source/backends/nes/NesBackend.cpp
	Source/backends/nes/NsfApuChip.cpp
	Source/backends/nes/NsfVrc6Chip.cpp
	Source/backends/nes/NsfVrc7Chip.cpp
	Source/backends/nes/NsfN163Chip.cpp
	Source/backends/nes/NsfPlayStubs.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/nes_apu.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/nes_dmc.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/nes_vrc6.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/nes_vrc7.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/nes_n106.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/legacy/emu2413.c
	Source/backends/nes/third_party/nsfplay/xgm/devices/CPU/nes_cpu.cpp
	Source/i18n/LanguageManager.cpp
	Source/i18n/LanguagePack.cpp
	Source/settings/SettingsStore.cpp
	Source/ui/CardComponent.cpp
	Source/ui/ChannelStrip.cpp
	Source/ui/ChipPanel.cpp
	Source/ui/DropHintOverlay.cpp
	Source/ui/FontChooserDialog.cpp
	Source/ui/IconButton.cpp
	Source/ui/Icons.cpp
	Source/ui/MixerPanel.cpp
	Source/ui/PlayView.cpp
	Source/ui/SettingsOverlay.cpp
	Source/ui/SongInfoPanel.cpp
	Source/ui/Theme.cpp
	Source/ui/ToyboxLookAndFeel.cpp
	Source/ui/UiFont.cpp
	Source/ui/SyncGroup.cpp
	Source/ui/TransportPanel.cpp
	Source/ui/VisualiserView.cpp
)

target_include_directories(VGMToybox PRIVATE Source)

# The standalone is the only wrapper that can meet another copy of itself, so it is the only one
# with an application class of its own: JUCE's default one always allows a second instance and
# ignores the command line it was started with (Source/Standalone/StandaloneApp.cpp says why).
# Defining the switch removes JUCE's class and asks that file for juce_CreateApplication() instead.
target_sources(VGMToybox_Standalone PRIVATE
	Source/Standalone/StandaloneApp.cpp
	Source/Standalone/TrayController.cpp
	Source/Standalone/TrayMenu.cpp
	Source/Standalone/TrayIcon.cpp)

# TrayIcon.cpp owns the icon, clicks and the JUCE fallback; the context menu itself is the one
# platform-specific piece. Windows uses Win32, macOS uses the NSStatusItem's native NSMenu, and
# Linux keeps the JUCE menu because its tray protocol has no native menu to attach.
target_sources(VGMToybox_Standalone PRIVATE Source/Standalone/TrayMenuIcons.cpp)

if(APPLE)
	target_sources(VGMToybox_Standalone PRIVATE Source/Standalone/TrayIcon_mac.mm)
elseif(WIN32)
	target_sources(VGMToybox_Standalone PRIVATE Source/Standalone/TrayIcon_windows.cpp)
else()
	target_sources(VGMToybox_Standalone PRIVATE Source/Standalone/TrayIcon_linux.cpp)
endif()

target_compile_definitions(VGMToybox_Standalone PRIVATE JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
target_include_directories(VGMToybox_Standalone PRIVATE Source)

# The tray icon is the application icon, not a second drawing. Embed the same PNGs the .ico and
# the macOS bundle are made from; the 16 px file is used when Windows asks for a 16 px tray image,
# so the notification area does not have to downscale the 256 px source.
juce_add_binary_data(VGMToyboxTrayIcons
	HEADER_NAME TrayIconData.h
	NAMESPACE TrayIconData
	SOURCES
		assets/icon/vgmtoybox-16.png
		assets/icon/vgmtoybox-256.png)
target_link_libraries(VGMToybox_Standalone PRIVATE VGMToyboxTrayIcons)

# The built-in translations are compiled into the shared plugin code, not installed next to a
# bundle: the VST3 and the standalone load the same packs, and a plugin host may copy a bundle
# without its neighbours. User translations are found at runtime in the settings directory.
juce_add_binary_data(VGMToyboxI18n
	HEADER_NAME I18nData.h
	NAMESPACE I18nData
	SOURCES
		assets/i18n/en.lang
		assets/i18n/zh-Hans.lang)
target_link_libraries(VGMToybox PUBLIC VGMToyboxI18n)

# The JUCE configuration *every* target in this project compiles with.
#
# JUCE compiles its modules into each target that uses them, so a define that sits on one target
# only is not a policy - it is a difference waiting for a machine that cares. JUCE_USE_CURL is the
# one that found one: it defaults to on for Linux, its juce_core.cpp then includes <curl/curl.h>,
# and the plugin was the only target saying no. vgm_core_test compiles juce_core.cpp itself and did
# not say it, so the CI's Ubuntu runner - which has no libcurl and no reason to grow one, since
# nothing here talks to the network - stopped there instead of at anything to do with this project.
# The targets that link VGMToybox inherit the list; the console ones are given it below.
set(TOYBOX_JUCE_DEFINITIONS
	JUCE_WEB_BROWSER=0
	JUCE_USE_CURL=0
	JUCE_VST3_CAN_REPLACE_VST2=0
	JUCE_DISPLAY_SPLASH_SCREEN=0
	JUCE_STRICT_REFCOUNTEDPOINTER=1
)

target_compile_definitions(VGMToybox PUBLIC
	${TOYBOX_JUCE_DEFINITIONS}
	TOYBOX_OUTPUT_BUSES=${TOYBOX_OUTPUT_BUSES}
	TOYBOX_GIT_COMMIT="${TOYBOX_GIT_COMMIT}"
)

target_link_libraries(VGMToybox PRIVATE
	# libvgm (built by the parent project, see the root CMakeLists.txt)
	${LIBVGM_TARGETS}
	juce::juce_audio_processors
	juce::juce_audio_utils
	juce::juce_recommended_config_flags
	juce::juce_recommended_lto_flags
)

# --- console test for the mixer engine (not built by default) ---
# Renders a VGM/VGZ file offline (plain player and per-channel mixer), writes the plain render
# to a WAV and checks the mixer acceptance criteria, e.g.:
#   cmake --build <build dir> --target vgm_mixer_test
#   <build dir>/plugin/vgm_mixer_test test.vgz 5
add_executable(vgm_mixer_test EXCLUDE_FROM_ALL
	Tests/PlaybackEngineTest.cpp
	Source/playback/BackendRegistry.cpp
	Source/playback/PlaybackEngine.cpp
	Source/playback/DawSync.cpp
	Source/playback/SessionState.cpp
	Source/settings/SettingsStore.cpp
	Source/chips/GbcApuDecoder.cpp
	Source/chips/HuC6280Decoder.cpp
	Source/chips/K051649Decoder.cpp
	Source/chips/NesApuDecoder.cpp
	Source/chips/NoteExtractor.cpp
	Source/chips/PokeyDecoder.cpp
	Source/chips/Saa1099Decoder.cpp
	Source/chips/WswanDecoder.cpp
	Source/chips/OplDecoder.cpp
	Source/chips/Ay8910Decoder.cpp
	Source/chips/Rf5c68Decoder.cpp
	Source/chips/X1_010Decoder.cpp
	Source/chips/C140Decoder.cpp
	Source/chips/C352Decoder.cpp
	Source/chips/K053260Decoder.cpp
	Source/chips/K054539Decoder.cpp
	Source/chips/QSoundDecoder.cpp
	Source/chips/Es5503Decoder.cpp
	Source/chips/K005289Decoder.cpp
	Source/chips/Msm5232Decoder.cpp
	Source/chips/MikeyDecoder.cpp
	Source/chips/MultiPcmDecoder.cpp
	Source/chips/K007232Decoder.cpp
	Source/chips/VsuDecoder.cpp
	Source/chips/Okim6295Decoder.cpp
	Source/chips/Ga20Decoder.cpp
	Source/chips/SegaPcmDecoder.cpp
	Source/chips/Ymf271Decoder.cpp
	Source/chips/Ymz280bDecoder.cpp
	Source/chips/Sn76489Decoder.cpp
	Source/chips/OpmDecoder.cpp
	Source/chips/OpnDecoder.cpp
	Source/backends/BuiltInBackends.cpp
	Source/backends/libvgm/LibvgmBackend.cpp
	Source/backends/libvgm/MidiSequenceBuilder.cpp
	Source/backends/libvgm/VgmCommandReader.cpp
	Source/formats/vgm/VgmSongLoader.cpp
	Source/formats/ftm/FtmSongLoader.cpp
	Source/formats/ftm/FtmSongIr.cpp
	Source/backends/libvgm/LibvgmSongInstance.cpp
	Source/backends/libvgm/LibvgmTrackPlayer.cpp
	Source/backends/libvgm/LibvgmVgz.cpp
	Source/backends/nes/NesBackend.cpp
	Source/backends/nes/NsfApuChip.cpp
	Source/backends/nes/NsfVrc6Chip.cpp
	Source/backends/nes/NsfVrc7Chip.cpp
	Source/backends/nes/NsfN163Chip.cpp
	Source/backends/nes/NsfPlayStubs.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/nes_apu.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/nes_dmc.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/nes_vrc6.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/nes_vrc7.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/nes_n106.cpp
	Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/legacy/emu2413.c
	Source/backends/nes/third_party/nsfplay/xgm/devices/CPU/nes_cpu.cpp
)
# --- console test for the chip decoders (not built by default) ---
# Register writes in, notes out. Links Source/chips and nothing else - no libvgm, no JUCE.
#   cmake --build <build dir> --target vgm_chip_test
#   <build dir>/plugin/vgm_chip_test
add_executable(vgm_chip_test EXCLUDE_FROM_ALL
	Source/backends/libvgm/VgmCommandReader.cpp
	Source/formats/vgm/VgmSongLoader.cpp
	Source/formats/ftm/FtmSongLoader.cpp
	Source/formats/ftm/FtmSongIr.cpp
	Source/formats/nsf/NsfFile.cpp
	Source/formats/nsf/NsfMemory.cpp
	Source/formats/ftm/compiler/Chunk.cpp
	Tests/Support/TestSupport.cpp
	Tests/Formats/FormatsTests.cpp
	Tests/ChipDecoderTest.cpp
	Source/chips/GbcApuDecoder.cpp
	Source/chips/HuC6280Decoder.cpp
	Source/chips/K051649Decoder.cpp
	Source/chips/NesApuDecoder.cpp
	Source/chips/NoteExtractor.cpp
	Source/chips/PokeyDecoder.cpp
	Source/chips/Saa1099Decoder.cpp
	Source/chips/WswanDecoder.cpp
	Source/chips/OplDecoder.cpp
	Source/chips/Ay8910Decoder.cpp
	Source/chips/Rf5c68Decoder.cpp
	Source/chips/X1_010Decoder.cpp
	Source/chips/C140Decoder.cpp
	Source/chips/C352Decoder.cpp
	Source/chips/K053260Decoder.cpp
	Source/chips/K054539Decoder.cpp
	Source/chips/QSoundDecoder.cpp
	Source/chips/Es5503Decoder.cpp
	Source/chips/K005289Decoder.cpp
	Source/chips/Msm5232Decoder.cpp
	Source/chips/MikeyDecoder.cpp
	Source/chips/MultiPcmDecoder.cpp
	Source/chips/K007232Decoder.cpp
	Source/chips/VsuDecoder.cpp
	Source/chips/Okim6295Decoder.cpp
	Source/chips/Ga20Decoder.cpp
	Source/chips/SegaPcmDecoder.cpp
	Source/chips/Ymf271Decoder.cpp
	Source/chips/Ymz280bDecoder.cpp
	Source/chips/Sn76489Decoder.cpp
	Source/chips/OpmDecoder.cpp
	Source/chips/OpnDecoder.cpp
)
# NSFPlay's chip cores, for the NES backend. Vendored under Source/backends/nes/third_party
# target yet): the plan is to drive them from the module compiler's register stream, which is what
# makes the expansion chips reachable at all - libvgm has no VRC6 and the VGM format has no command
# for one. See docs/review/2026-10-24-nes-backend-plan.md.
set(NSFPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Source/backends/nes/third_party/nsfplay")
target_sources(vgm_chip_test PRIVATE
	Source/backends/nes/NsfApuChip.cpp
	Source/backends/nes/NsfVrc6Chip.cpp
	Source/backends/nes/NsfVrc7Chip.cpp
	Source/backends/nes/NsfN163Chip.cpp
	Source/backends/nes/NsfPlayStubs.cpp
	"${NSFPLAY_DIR}/xgm/devices/Sound/nes_apu.cpp"
	"${NSFPLAY_DIR}/xgm/devices/Sound/nes_vrc6.cpp"
	"${NSFPLAY_DIR}/xgm/devices/Sound/nes_vrc7.cpp"
	"${NSFPLAY_DIR}/xgm/devices/Sound/nes_n106.cpp"
	"${NSFPLAY_DIR}/xgm/devices/Sound/legacy/emu2413.c"
	"${NSFPLAY_DIR}/xgm/devices/Sound/nes_dmc.cpp"
	"${NSFPLAY_DIR}/xgm/devices/CPU/nes_cpu.cpp"
)
target_include_directories(vgm_chip_test PRIVATE Source
)

target_include_directories(vgm_chip_test PRIVATE Source)
# the chips sources are C++17 (nested namespaces, std::clamp); the plugin target gets the same
# standard from JUCE's own flags, this target has no JUCE
target_compile_features(vgm_chip_test PRIVATE cxx_std_17)

# --- the OPN decoder against libvgm's own YM2612 core (not built by default) ---
# The unit test above states its expectations from memory of the register map; this one asks the
# chip. libvgm's core plays the same register stream with all channels muted but one, so "this
# channel sounds" is a measurement, and the decoder has to answer the same question from the same
# writes. It links libvgm, which is why it is a target of its own.
#   cmake --build <build dir> --target vgm_opn_test
#   <build dir>/plugin/vgm_opn_test
add_executable(vgm_opn_test EXCLUDE_FROM_ALL
	Tests/OpnCoreOracleTest.cpp
	Source/chips/OpnDecoder.cpp
)
target_include_directories(vgm_opn_test PRIVATE Source)
target_compile_features(vgm_opn_test PRIVATE cxx_std_17)
target_link_libraries(vgm_opn_test PRIVATE ${LIBVGM_TARGETS})

# --- console test for the playback core (not built by default) ---
# Drives PlaybackEngine through the fake tone backend, and the settings store through a temporary
# directory. Deliberately links **no libvgm code**: if the core still depended on the format
# engine, this target would not link.
#   cmake --build <build dir> --target vgm_core_test
#   <build dir>/plugin/vgm_core_test
add_executable(vgm_core_test EXCLUDE_FROM_ALL
	Tests/PlaybackCoreTest.cpp
	Source/playback/BackendRegistry.cpp
	Source/playback/PlaybackEngine.cpp
	Source/playback/DawSync.cpp
	Source/playback/HostClockSource.cpp
	Source/playback/SessionState.cpp
	Source/backends/fake/FakeBackend.cpp
	Source/i18n/LanguageManager.cpp
	Source/i18n/LanguagePack.cpp
	Source/settings/SettingsStore.cpp
	Source/Standalone/TrayController.cpp
	Source/Standalone/TrayMenu.cpp
)
target_include_directories(vgm_core_test PRIVATE Source)
target_compile_definitions(vgm_core_test PRIVATE
	${TOYBOX_JUCE_DEFINITIONS}
	TOYBOX_OUTPUT_BUSES=${TOYBOX_OUTPUT_BUSES}
	JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(vgm_core_test PRIVATE
	juce::juce_core
	juce::juce_audio_basics
	# for juce::PropertiesFile, the file the settings live in (Source/settings)
	juce::juce_data_structures
	# for juce::MessageManager, which PlaybackEngine.cpp compiles against for the message-thread
	# half of an asynchronous load (review M-1). The tests never call that path - it needs a
	# running message loop - but the module has to be on the include path to compile the file.
	juce::juce_events
	juce::juce_recommended_config_flags
	# the tray menu it drives is translated; the packs are compiled into the plugin code
	VGMToyboxI18n
)

# --- console test for the button faces (not built by default) ---
# Renders ui/ButtonPaint.h into images and checks the invariants docs/ui-button-design.md
# promises: the shadow below a face, the rounded corner, the weaker shadow while pressed, the
# accent ring on focus, the card-painted glow of a toggled button. It needs the ui/ painting code
# and nothing else - no engine, no libvgm. ui/Theme.cpp comes with it: the palette stopped being
# inline constants in the header when the theme became switchable, so the colours are defined in a
# translation unit now and a test that checks pixels has to compile it.
#   cmake --build <build dir> --target vgm_ui_test
#   <build dir>/plugin/vgm_ui_test
add_executable(vgm_ui_test EXCLUDE_FROM_ALL
	Tests/UiButtonTest.cpp
	Source/ui/IconButton.cpp
	Source/ui/Icons.cpp
	Source/ui/Theme.cpp
)
target_include_directories(vgm_ui_test PRIVATE Source)
target_compile_definitions(vgm_ui_test PRIVATE
	${TOYBOX_JUCE_DEFINITIONS}
	JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(vgm_ui_test PRIVATE
	juce::juce_gui_basics
	juce::juce_recommended_config_flags
)

# --- console harness for the window (not built by default) --------------------------------------
# Phase 0 of docs/desgin/testing-architecture.md: can the editor be built and laid out without a
# window, and can its structure be read as text? It links the plugin's shared code - the STATIC
# target juce_add_plugin creates - so it drives the same PluginEditor the application uses, and the
# command and assertion language grows on top of what is here.
#   cmake --build <build dir> --target vgm_app_test
#   <build dir>/plugin/vgm_app_test
add_executable(vgm_app_test EXCLUDE_FROM_ALL
	Tests/AppHarnessTest.cpp
)
target_include_directories(vgm_app_test PRIVATE Source)
# Every test executable builds with TOYBOX_TEST_BUILD, which makes SettingsStore refuse to open the
# machine's own settings file (see SettingsStore::settingsDirectory). A test main that forgets to
# point the store at a throwaway folder then fails loudly instead of quietly reading - and being
# decided by - the preferences of whoever runs it.
foreach (toyboxTestTarget vgm_mixer_test vgm_chip_test vgm_opn_test vgm_core_test vgm_ui_test vgm_app_test)
	if (TARGET ${toyboxTestTarget})
		target_compile_definitions(${toyboxTestTarget} PRIVATE TOYBOX_TEST_BUILD=1)
	endif()
endforeach()
# The only difference from the plugin it drives: the message loop. JUCE gates
# MessageManager::runDispatchLoopUntil() on this macro, and without it a script can see nothing but
# what a command sets up synchronously - the layout an AsyncUpdater does (a TreeView recalculating
# its rows), an async change broadcast, Button::triggerClick() - which is a lot of the user
# interface. The macro only adds that one declaration to MessageManager (no data members, no
# virtuals), so it cannot disagree with the plugin's copy of the headers about anything else.
target_compile_definitions(vgm_app_test PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(vgm_app_test PRIVATE
	VGMToybox
	juce::juce_audio_processors
	juce::juce_audio_utils
	juce::juce_recommended_config_flags
)

target_include_directories(vgm_mixer_test PRIVATE Source)
target_compile_definitions(vgm_mixer_test PRIVATE
	${TOYBOX_JUCE_DEFINITIONS}
	TOYBOX_OUTPUT_BUSES=${TOYBOX_OUTPUT_BUSES}
	JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(vgm_mixer_test PRIVATE
	${LIBVGM_TARGETS}
	juce::juce_core
	juce::juce_audio_basics
	# PlaybackEngine.cpp compiles the MessageManager call for asynchronous loads (review M-1)
	juce::juce_events
	# settings/SettingsStore.cpp uses juce::PropertiesFile: the song loader reads the sample-root
	# preference before it builds the MIDI sequence (design doc section 15)
	juce::juce_data_structures
	juce::juce_recommended_config_flags
)

# --- CTest -------------------------------------------------------------------------------------
# One test per console program and one per window script. build.ps1 -Test already ran the same
# list by hand; registering it here means a failure names the script, `ctest -R app.` reruns only
# those, and CI can upload a JUnit file instead of parsing our own output.
#
# CTest does not build anything: the test targets are EXCLUDE_FROM_ALL, so build them first
# (build.ps1 -Test does). build.ps1 -TestSong writes TOYBOX_TEST_SONG here, which keeps the song
# the scripts load in one place; the repository carries one, so plain `ctest` works too.
set(TOYBOX_TEST_SONG "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/minimal.vgm" CACHE FILEPATH
	"Song loaded by the console tests (build.ps1 -TestSong overrides it)")

if(EXISTS "${TOYBOX_TEST_SONG}")
	get_filename_component(_toybox_test_song "${TOYBOX_TEST_SONG}" ABSOLUTE)
	set(_toybox_test_env "VGM_TEST_SONG=${_toybox_test_song}")
endif()

add_test(NAME chip COMMAND vgm_chip_test)
add_test(NAME core COMMAND vgm_core_test)
add_test(NAME ui   COMMAND vgm_ui_test)
add_test(NAME opn  COMMAND vgm_opn_test)
set_tests_properties(core ui PROPERTIES LABELS "unit" TIMEOUT 120)
set_tests_properties(opn PROPERTIES LABELS "unit" TIMEOUT 300)

if(_toybox_test_env)
	add_test(NAME engine COMMAND vgm_mixer_test "${_toybox_test_song}" 3)
	set_tests_properties(engine PROPERTIES LABELS "integration" TIMEOUT 300)
endif()

# The WonderSwan decoder has no song in test_file/ (that folder is Neo Geo, NES, Sega CD and Mega
# Drive), so its end-to-end path runs on the generated asset: the register parsing, the note table
# and the per-channel tracks would otherwise only ever be exercised by the hand-run mixer test. Three
# seconds, not two: the sweep on Ch 3 runs to 1.88 s and the check names its last step.
set(_toybox_wswan_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/wswan.vgm")
if(EXISTS "${_toybox_wswan_song}")
	add_test(NAME engine.wswan COMMAND vgm_mixer_test "${_toybox_wswan_song}" 3)
	set_tests_properties(engine.wswan PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# The three generated assets below were hand-run only until the per-note expectations existed in
# PlaybackEngineTest: a build could break the AY mixer mask, the OPL4 second bank or the YM2610 ADPCM
# trigger and every ctest entry stayed green, because no test ever loaded those files. Each one now
# asserts the notes its generator writes down, so they belong in the suite.
foreach(_toybox_asset psg opl4 ym2610 fm_alg0)
	set(_toybox_asset_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/${_toybox_asset}.vgm")
	if(EXISTS "${_toybox_asset_song}")
		add_test(NAME "engine.${_toybox_asset}" COMMAND vgm_mixer_test "${_toybox_asset_song}" 3)
		set_tests_properties("engine.${_toybox_asset}" PROPERTIES LABELS "integration" TIMEOUT 120)
	endif()
endforeach()

# The YM2612's DAC channel needs a 0x67 PCM data block before it can say anything, which no song in
# test_file/ guarantees: the generated asset alternates 50 ms of samples with 50 ms of silence, so
# "the DAC track reports notes" means the bank was read from its first byte to its last.
set(_toybox_dac_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/dac.vgm")
if(EXISTS "${_toybox_dac_song}")
	add_test(NAME engine.dac COMMAND vgm_mixer_test "${_toybox_dac_song}" 2)
	set_tests_properties(engine.dac PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# PlaybackEngineTest [12] needs a song with its own loop point, and no asset had one: the check
# printed "loop check skipped" on every run since it was written. loop.vgm declares one in the
# header (offset 0x1C, length 0x20) and plays twenty notes per pass, so a second pass that sends
# no notes is a failure rather than a missing precondition.
set(_toybox_loop_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/loop.vgm")
if(EXISTS "${_toybox_loop_song}")
	add_test(NAME engine.loop COMMAND vgm_mixer_test "${_toybox_loop_song}" 2)
	set_tests_properties(engine.loop PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# The X1-010 (M2): a chip whose sample data lives in ROM, so the core stays silent until a ROM data
# block allocates it - a register-only asset would prove the decoder but not that its notes land on
# an audible track. x1.vgm uploads a wavetable and an envelope through 0xC8 writes, the same command
# the notes travel on.
set(_toybox_x1_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/x1.vgm")
if(EXISTS "${_toybox_x1_song}")
	add_test(NAME engine.x1 COMMAND vgm_mixer_test "${_toybox_x1_song}" 2)
	set_tests_properties(engine.x1 PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# The YMF271 (M3): 0xD1 carries the chip in bit 7 of the port byte and the register bank in the low
# seven bits, and under sync mode 0 only a bank-0 write reaches the other three operators. A decoder
# that ignored the bank would still pass its own unit test, so opx.vgm goes through the file path:
# two four-operator groups, one an octave above the other, whose notes have to land on two tracks.
set(_toybox_opx_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/opx.vgm")
if(EXISTS "${_toybox_opx_song}")
	add_test(NAME engine.opx COMMAND vgm_mixer_test "${_toybox_opx_song}" 2)
	set_tests_properties(engine.opx PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# The C140 (M3): the first voice whose pitch is a playback rate rather than a note, and a chip that
# returns silence until its ROM block is allocated. c140.vgm gives two voices an octave apart and
# ends their notes in the two ways the chip offers - voice 0 by its end address, with no key-off
# written at all, voice 1 by key-off under the loop bit - so four audible notes per voice means both
# length rules survived the trip through the file.
set(_toybox_c140_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/c140.vgm")
if(EXISTS "${_toybox_c140_song}")
	add_test(NAME engine.c140 COMMAND vgm_mixer_test "${_toybox_c140_song}" 2)
	set_tests_properties(engine.c140 PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# The C352 (M4): a sampled player like the C140, but its key-on is a separate write (offset 0x202)
# rather than the flag write, and its registers are whole 16-bit words delivered by one 0xE1
# command. c352.vgm keys two voices on a single execute write - voice 0 non-looping, ended by its
# end address, voice 16 looping, ended by the key-off - and puts the second voice in ROM bank 1,
# where only `bank << 16 | start` finds it. Four audible notes per voice therefore needs the
# execute write, both bytes of the offset and both halves of every value.
set(_toybox_c352_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/c352.vgm")
if(EXISTS "${_toybox_c352_song}")
	add_test(NAME engine.c352 COMMAND vgm_mixer_test "${_toybox_c352_song}" 2)
	set_tests_properties(engine.c352 PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# The QSound (M4): no key bit at all - a voice sounds while its rate and its volume are non-zero, so
# the note starts on the rising edge of the rate register and ends when it is written back to zero.
# qsound.vgm keys two voices four times each that way: voice 0 a 55-byte periodic waveform that has
# to come out as A4, voice 1 noise that has to come out as the C4 drum trigger. A decoder looking for
# a key register plays nothing; one that ignores the zero write never stops.
set(_toybox_qsound_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/qsound.vgm")
if(EXISTS "${_toybox_qsound_song}")
	add_test(NAME engine.qsound COMMAND vgm_mixer_test "${_toybox_qsound_song}" 2)
	set_tests_properties(engine.qsound PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# The GA20 (M4): four voices, a start address in 16-byte units, a rate that is a divider and a sample
# that ends on a 0x00 byte. ga20.vgm keys two voices four times each: voice 0 a 16-byte periodic
# waveform that has to come out as G4, voice 1 noise that has to come out as the C4 drum trigger. A
# decoder treating the start registers as a byte address reads past the sample and plays nothing.
set(_toybox_ga20_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/ga20.vgm")
if(EXISTS "${_toybox_ga20_song}")
	add_test(NAME engine.ga20 COMMAND vgm_mixer_test "${_toybox_ga20_song}" 2)
	set_tests_properties(engine.ga20 PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# The pitch audit (P4 in section 15.7): per track, how many notes were register-exact, how many
# needed a measured root and how many are one-shot triggers. It does not fail on a track with several
# roots - a voice that plays several samples has one per sample - so it is a report, and the cross-rate
# property is asserted in vgm_chip_test where the sample is known.
foreach(_toybox_pitch_asset k053260 es5503 ga20)
	set(_toybox_pitch_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/${_toybox_pitch_asset}.vgm")
	if(EXISTS "${_toybox_pitch_song}")
		add_test(NAME pitch.${_toybox_pitch_asset}
				 COMMAND vgm_mixer_test "${_toybox_pitch_song}" 2 --pitch-report)
		set_tests_properties(pitch.${_toybox_pitch_asset} PROPERTIES LABELS "integration" TIMEOUT 120)
	endif()
endforeach()

# The ES5503 (M4): a wavetable chip - the note is the table's repeat rate, so a decoder that reads
# the table size instead of its measured cycle is a few octaves out, and one that uses the output
# rate as the chip's rate (`clock / 8 / (enabled + 2)`) is out by more. es5503.vgm keys two
# oscillators four times each: a 1024-byte single-cycle square at frequency 8,000 has to come out as
# G4, a one-shot noise table has to come out as the C4 drum trigger, and the halt bit has to end
# each note.
set(_toybox_es5503_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/es5503.vgm")
if(EXISTS "${_toybox_es5503_song}")
	add_test(NAME engine.es5503 COMMAND vgm_mixer_test "${_toybox_es5503_song}" 2)
	set_tests_properties(engine.es5503 PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# The Sega PCM (M4): the registers ARE the playback state - current address, pitch, end address and
# one control bit - and the command writes one byte of them at a time (0xC0). segapcm.vgm keys two
# voices four times each: voice 0 a 16-byte periodic waveform at pitch 29 that has to come out as A3,
# voice 1 noise that has to come out as the C4 drum trigger. A decoder that reads the start address as
# a byte address looks two hundred and fifty-six times too far into the ROM and plays nothing.
set(_toybox_segapcm_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/segapcm.vgm")
if(EXISTS "${_toybox_segapcm_song}")
	add_test(NAME engine.segapcm COMMAND vgm_mixer_test "${_toybox_segapcm_song}" 2)
	set_tests_properties(engine.segapcm PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# The K054539 (M4): a sample ends on a marker byte (0x80), the key registers are sixteen-bit
# addresses (0x214/0x215 delivered by one 0xD3 command), and the volume register attenuates rather
# than amplifies. k054539.vgm keys two voices four times each: voice 0 a periodic waveform that has
# to come out as A4, voice 1 noise that has to come out as the C4 drum trigger, both ended by their
# own 0x80 marker. A decoder that read the volume as a level, kept only the low half of the offset
# or ignored the marker would fail on the pitch, the count or the timing.
set(_toybox_k054539_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/k054539.vgm")
if(EXISTS "${_toybox_k054539_song}")
	add_test(NAME engine.k054539 COMMAND vgm_mixer_test "${_toybox_k054539_song}" 2)
	set_tests_properties(engine.k054539 PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# The K053260 (M4): key-on is a rising edge in a shared mask register (offset 0x28), not a write to
# the voice, and the chip's length register ends a note without any key-off. k053260.vgm keys two
# voices four times each with that one mask write - voice 0 a periodic waveform that has to come out
# as A4, voice 1 noise that has to come out as the C4 drum trigger. A decoder that keyed on a voice
# write, ignored the mask's edges or invented a pitch for the noise would fail on the note count, the
# timing or the pitch.
set(_toybox_k053260_song "${CMAKE_CURRENT_SOURCE_DIR}/Tests/assets/k053260.vgm")
if(EXISTS "${_toybox_k053260_song}")
	add_test(NAME engine.k053260 COMMAND vgm_mixer_test "${_toybox_k053260_song}" 2)
	set_tests_properties(engine.k053260 PROPERTIES LABELS "integration" TIMEOUT 120)
endif()

# One test per script in Tests/scripts, discovered rather than listed: adding a script must not mean
# editing this file (and a stale hard-coded list would silently stop running a script).
file(GLOB _toybox_scripts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/scripts/*.txt")
list(SORT _toybox_scripts)

foreach(_script_file IN LISTS _toybox_scripts)
	get_filename_component(_script_name "${_script_file}" NAME_WE)

	add_test(NAME "app.${_script_name}" COMMAND vgm_app_test "${_script_file}")

	# The scripts name the assets they use from the repository root (`plugin/Tests/...`), and
	# the settings directives do the same. CTest's default is the build directory, so pin the
	# working directory to the root the scripts (and their docs) already assume.
	if(_toybox_test_env)
		set_tests_properties("app.${_script_name}" PROPERTIES
			LABELS app
			TIMEOUT 120
			ENVIRONMENT "${_toybox_test_env}"
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
	else()
		set_tests_properties("app.${_script_name}" PROPERTIES
			LABELS app
			TIMEOUT 120
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
	endif()
endforeach()
