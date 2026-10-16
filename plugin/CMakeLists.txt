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
	FORMATS VST3 Standalone ${TOYBOX_AU_FORMAT}
	IS_SYNTH TRUE
	NEEDS_MIDI_INPUT FALSE
	NEEDS_MIDI_OUTPUT FALSE
	IS_MIDI_EFFECT FALSE
	EDITOR_WANTS_KEYBOARD_FOCUS TRUE
	COPY_PLUGIN_AFTER_BUILD FALSE
	VST3_CATEGORIES "Instrument"
)

target_sources(VGMToybox PRIVATE
	Source/PluginProcessor.cpp
	Source/PluginEditor.cpp
	Source/playback/BackendRegistry.cpp
	Source/playback/PlaybackEngine.cpp
	Source/backends/BuiltInBackends.cpp
	Source/backends/libvgm/LibvgmBackend.cpp
	Source/backends/libvgm/LibvgmSongInstance.cpp
	Source/backends/libvgm/LibvgmTrackPlayer.cpp
	Source/backends/libvgm/LibvgmVgz.cpp
	Source/ui/CardComponent.cpp
	Source/ui/ChannelStrip.cpp
	Source/ui/ChipPanel.cpp
	Source/ui/DropHintOverlay.cpp
	Source/ui/IconButton.cpp
	Source/ui/Icons.cpp
	Source/ui/LightLookAndFeel.cpp
	Source/ui/MixerPanel.cpp
	Source/ui/PlayView.cpp
	Source/ui/SongInfoPanel.cpp
	Source/ui/TransportPanel.cpp
	Source/ui/VisualiserView.cpp
)

target_include_directories(VGMToybox PRIVATE Source)

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
	Source/backends/BuiltInBackends.cpp
	Source/backends/libvgm/LibvgmBackend.cpp
	Source/backends/libvgm/LibvgmSongInstance.cpp
	Source/backends/libvgm/LibvgmTrackPlayer.cpp
	Source/backends/libvgm/LibvgmVgz.cpp
)
# --- console test for the playback core (not built by default) ---
# Drives PlaybackEngine through the fake tone backend. Deliberately links **no libvgm code**: if
# the core still depended on the format engine, this target would not link.
#   cmake --build <build dir> --target vgm_core_test
#   <build dir>/plugin/vgm_core_test
add_executable(vgm_core_test EXCLUDE_FROM_ALL
	Tests/PlaybackCoreTest.cpp
	Source/playback/BackendRegistry.cpp
	Source/playback/PlaybackEngine.cpp
	Source/backends/fake/FakeBackend.cpp
)
target_include_directories(vgm_core_test PRIVATE Source)
target_compile_definitions(vgm_core_test PRIVATE
	${TOYBOX_JUCE_DEFINITIONS}
	TOYBOX_OUTPUT_BUSES=${TOYBOX_OUTPUT_BUSES}
	JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(vgm_core_test PRIVATE
	juce::juce_core
	juce::juce_audio_basics
	juce::juce_recommended_config_flags
)

# --- console test for the button faces (not built by default) ---
# Renders ui/ButtonPaint.h into images and checks the invariants docs/ui-button-design.md
# promises: the shadow below a face, the rounded corner, the weaker shadow while pressed, the
# accent ring on focus, the card-painted glow of a toggled button. It needs the ui/ painting code
# and nothing else - no engine, no libvgm.
#   cmake --build <build dir> --target vgm_ui_test
#   <build dir>/plugin/vgm_ui_test
add_executable(vgm_ui_test EXCLUDE_FROM_ALL
	Tests/UiButtonTest.cpp
	Source/ui/IconButton.cpp
	Source/ui/Icons.cpp
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
	juce::juce_recommended_config_flags
)
