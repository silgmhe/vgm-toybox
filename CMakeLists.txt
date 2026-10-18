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
#
# The version here is the **only** place a version lives - semantic versioning, MAJOR.MINOR.PATCH
# (docs/desgin/maintenance-policy.md section 6). Everything downstream reads it from here: JUCE
# turns it into JucePlugin_VersionString (Help > About), the VST3 module info and the Windows file
# version, and the release workflow refuses a tag that does not match it.
project(vgmtoybox VERSION 0.3.0 LANGUAGES C CXX)

# CTest is the runner for the console tests (plugin/Tests); their add_test() calls sit next to the
# targets in plugin/CMakeLists.txt. It does not mean the tests are built: they are EXCLUDE_FROM_ALL,
# so build.ps1 -Test builds them first and then runs `ctest`.
enable_testing()

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

# --- position independent code ---
# Everything built in this tree can end up inside the plugin, which is a shared object on macOS and
# Linux - and libvgm's zlib was the one archive that was not built for that. Linking it in gave, on
# the first Linux plugin build in CI:
#   /usr/bin/ld: libz.a(zutil.c.o): relocation R_X86_64_PC32 against symbol `z_errmsg' can not be
#                 used when making a shared object; recompile with -fPIC
# libvgm's own four libraries set this property for themselves; zlib does not, and libvgm fetches it
# with CPM and adds it as a sub-project of this tree, so the variable set here reaches it. On Windows
# and macOS this is what the platform does anyway or asks for, so the setting only costs the
# position-independent code generation on Linux - which the shared plugin needs regardless.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- the Linux C++ runtime, linked in ---
# The Linux packages are built inside an Ubuntu 22.04 container (see release.yml), so they ask for no
# more than that glibc - and glibc is backwards compatible, which is the point of building there. The
# C++ runtime is a different matter: libstdc++ carries its own symbol versions (GLIBCXX_3.4.x) tied to
# the compiler that produced it, so a machine can meet the glibc requirement and still fail on
# libstdc++. Linking it in removes that variable, at about two megabytes per bundle.
#
# glibc itself stays shared, on purpose: it is what dlopen, ALSA's plugins and the X11 drivers resolve
# against, and a static glibc inside a shared object is not something that works. That is also why a
# fully static build is not an option here - the plugin is a .so the host loads.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	add_link_options(-static-libstdc++ -static-libgcc)
endif()

# --- the macOS deployment target, stated rather than inherited ---
# Left unset, CMake passes no -mmacosx-version-min and clang falls back to the version of the SDK it
# is told to use, so the binaries run on nothing older than the build machine. The v0.0.4 bundles
# came off a macOS 26 runner with minos 26.0 for both slices - fine if every user is on the newest
# release, useless otherwise. 10.15 is the oldest macOS JUCE 8 supports; the arm64 slice cannot go
# below 11.0, and clang raises it there on its own.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
	# FORCE matters: project() has already created this as an empty cache entry, and without it the
	# set() would be a no-op that looks like it worked - which is how the first attempt at this
	# passed configure and still shipped whatever the SDK defaulted to.
	set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "minimum macOS version to build for" FORCE)
endif()

# --- libvgm ---
# Build the in-tree libvgm copy as part of this project (one-shot build, static libraries).
# The libraries under libs/ are vendored source, not submodules, so they can be patched in place -
# and if the copy is missing altogether, fall back to an installed libvgm package.
if(EXISTS "${PROJECT_SOURCE_DIR}/libs/libvgm/CMakeLists.txt")
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
	add_subdirectory(libs/libvgm)
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
