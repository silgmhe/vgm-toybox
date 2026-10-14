#### VGMPlay JUCE plugin (VST3 + Standalone) ####
# Builds the GUI/plugin front-end of VGMPlay. The audio rendering is done by the same libvgm
# libraries that the command-line player uses (vgm-player/vgm-emu/vgm-utils, no audio drivers).

# --- JUCE (fetched with CPM) ---
set(JUCE_TAG "8.0.9" CACHE STRING "JUCE version to use")
set(JUCE_MIRRORS "https://github.com/juce-framework/JUCE.git;https://gitee.com/mirrors/JUCE.git"
	CACHE STRING "git repositories to probe for JUCE (in order of preference)")
set(JUCE_REPOSITORY "" CACHE STRING "git repository to fetch JUCE from (empty: probe JUCE_MIRRORS)")

if(NOT TARGET juce_audio_processors)
	# CPM lives next to libvgm; keep the path relative to this directory, so that IDEs can
	# resolve it without knowing the layout of the build tree
	set(_cpm_module "${CMAKE_CURRENT_SOURCE_DIR}/../libvgm/cmake/CPM.cmake")
	if(NOT EXISTS "${_cpm_module}")
		message(FATAL_ERROR "vgmplay: '${_cpm_module}' not found - run 'git submodule update --init --recursive'")
	endif()

	# pick the first reachable repository (GitHub is not accessible from everywhere)
	if(NOT JUCE_REPOSITORY)
		foreach(_juce_repo IN LISTS JUCE_MIRRORS)
			message(STATUS "vgmplay: probing JUCE repository ${_juce_repo} ...")
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
		message(FATAL_ERROR "vgmplay: no reachable JUCE repository - "
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

# --- plugin target ---
juce_add_plugin(VGMPlay
	COMPANY_NAME "VGMPlay"
	PLUGIN_MANUFACTURER_CODE Vgmp
	PLUGIN_CODE Vgm1
	PRODUCT_NAME "VGMPlay"
	DESCRIPTION "VGM/VGZ player"
	FORMATS VST3 Standalone
	IS_SYNTH TRUE
	NEEDS_MIDI_INPUT FALSE
	NEEDS_MIDI_OUTPUT FALSE
	IS_MIDI_EFFECT FALSE
	EDITOR_WANTS_KEYBOARD_FOCUS TRUE
	COPY_PLUGIN_AFTER_BUILD FALSE
	VST3_CATEGORIES "Instrument"
)

target_sources(VGMPlay PRIVATE
	Source/PluginProcessor.cpp
	Source/PluginEditor.cpp
	Source/ui/CardComponent.cpp
	Source/ui/ChipPanel.cpp
	Source/ui/DropHintOverlay.cpp
	Source/ui/IconButton.cpp
	Source/ui/Icons.cpp
	Source/ui/LightLookAndFeel.cpp
	Source/ui/SongInfoPanel.cpp
	Source/ui/TransportPanel.cpp
	Source/vgm/VgmEngine.cpp
)

target_include_directories(VGMPlay PRIVATE Source)

target_compile_definitions(VGMPlay PUBLIC
	JUCE_WEB_BROWSER=0
	JUCE_USE_CURL=0
	JUCE_VST3_CAN_REPLACE_VST2=0
	JUCE_DISPLAY_SPLASH_SCREEN=0
	JUCE_STRICT_REFCOUNTEDPOINTER=1
)

target_link_libraries(VGMPlay PRIVATE
	# libvgm (built by the parent project, see the root CMakeLists.txt)
	${LIBVGM_TARGETS}
	juce::juce_audio_processors
	juce::juce_audio_utils
	juce::juce_recommended_config_flags
	juce::juce_recommended_lto_flags
)

# --- console test for the playback engine (not built by default) ---
# Renders a VGM/VGZ file offline into a WAV file, e.g.:
#   cmake --build <build dir> --target vgm_engine_test
#   <build dir>/plugin/vgm_engine_test test.vgz 5
add_executable(vgm_engine_test EXCLUDE_FROM_ALL
	Tests/VgmEngineTest.cpp
	Source/vgm/VgmEngine.cpp
)
target_include_directories(vgm_engine_test PRIVATE Source)
target_compile_definitions(vgm_engine_test PRIVATE JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(vgm_engine_test PRIVATE
	${LIBVGM_TARGETS}
	juce::juce_core
	juce::juce_audio_basics
	juce::juce_recommended_config_flags
)
