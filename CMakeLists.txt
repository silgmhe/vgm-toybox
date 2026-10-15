#### VGM Toybox (libvgm-based) ####
# Two front-ends share one in-tree libvgm build:
#   vgmplayer/ - the upstream command-line player (VGMPlay, its target and files unchanged)
#   plugin/    - VGM Toybox, the JUCE GUI plugin (VST3 + standalone)
# This file only sets the project up and decides what gets built; the targets live in the
# subdirectories.
# minimum 3.14 is required by CPM.cmake (used by libvgm's zlib dependency)
cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0042)
	cmake_policy(SET CMP0042 NEW)
endif()
if(POLICY CMP0048)
	cmake_policy(SET CMP0048 NEW)
endif()
# The project is the umbrella for both front-ends, so it carries the product name. The player
# subdirectory still names its own target "VGMPlay64" - the upstream identity lives there.
project(vgmtoybox VERSION 0.5 LANGUAGES C CXX)

#set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
#set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
#set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/libs/cmake_modules/")

# --- static MSVC runtime (/MT) ---
# Link the C/C++ runtime statically, so that the resulting executable only depends on
# Windows system DLLs instead of requiring the VC++ Redistributable to be installed.
# (Use CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL to link against the runtime DLLs.)
if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING
		"MSVC runtime library to use (MultiThreaded = static, MultiThreadedDLL = dynamic)")
endif()

# --- libvgm ---
# Build the libvgm submodule as part of this project (one-shot build, static libraries).
# If the submodule is not checked out, fall back to an installed libvgm package.
if(EXISTS "${PROJECT_SOURCE_DIR}/libvgm/CMakeLists.txt")
	# defaults for the in-tree libvgm build
	set(LIBRARY_TYPE "STATIC" CACHE STRING "libvgm: library build type (STATIC/SHARED)")
	set(BUILD_TESTS OFF CACHE BOOL "libvgm: build test programs")
	set(BUILD_PLAYER OFF CACHE BOOL "libvgm: build the libvgm command-line player")
	set(BUILD_VGM2WAV OFF CACHE BOOL "libvgm: build the vgm2wav tool")
	if(WIN32)
		# Use the WinAPI for charset conversion: the libiconv bundled with libvgm
		# is a DLL import library and would add a runtime dependency.
		set(UTIL_CHARCNV_ICONV OFF CACHE BOOL "libvgm: charset conversion via iconv")
		set(UTIL_CHARCNV_WINAPI ON CACHE BOOL "libvgm: charset conversion via the WinAPI")
	endif()
	add_subdirectory(libvgm)
	set(LIBVGM_TARGETS vgm-utils vgm-audio vgm-emu vgm-player)
else()
	find_package(libvgm REQUIRED)
	set(LIBVGM_TARGETS libvgm::vgm-utils libvgm::vgm-audio libvgm::vgm-emu libvgm::vgm-player)
endif()

# --- the command-line player (upstream VGMPlay) ---
add_subdirectory(vgmplayer)

# --- the JUCE-based GUI plugin (VGM Toybox) ---
# Uses the same libvgm libraries as the command-line player. JUCE itself is fetched with
# CPM by plugin/CMakeLists.txt (see JUCE_TAG / JUCE_REPOSITORY there).
option(TOYBOX_BUILD_PLUGIN "build the JUCE-based GUI plugin (VST3 + standalone)" ON)
if(TOYBOX_BUILD_PLUGIN)
	add_subdirectory(plugin)
endif()
