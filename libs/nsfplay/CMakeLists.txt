# NSFPlay's device cores, built as one static library.
#
# Licence: README-nsfplay.txt states "You may reuse this code without restriction", so the sources
# ship inside the product the way libvgm's do - no runtime loading, no optional component.
#
# What is built is the subset a player needs: the 6502, the memory map, the APU and DPCM, the
# expansion chips, and the two Yamaha-derived emulators those depend on. The loop detector, the CPU
# logger and the module model are left out: they are tools of the original application, not of
# playback. vgm_nsf_* targets supply the two logger symbols NES_CPU references.
#
# Vendored sources are compiled with warnings off. They predate this project's warning settings and
# bringing them up to it is not the job here; anything of ours that uses them is compiled normally.

add_library(nsfplay_core STATIC
	${CMAKE_CURRENT_SOURCE_DIR}/vcm/group.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/vcm/value.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Audio/rconv.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/CPU/nes_cpu.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Memory/nes_bank.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Memory/nes_mem.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Memory/nsf2_vectors.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Memory/ram64k.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Misc/nsf2_irq.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Sound/legacy/emu2149.c
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Sound/legacy/emu2413.c
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Sound/nes_apu.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Sound/nes_dmc.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Sound/nes_fds.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Sound/nes_fme7.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Sound/nes_mmc5.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Sound/nes_n106.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Sound/nes_vrc6.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/xgm/devices/Sound/nes_vrc7.cpp
)

# The include root is the vendored tree itself: the sources include each other as "xgm/devices/...".
target_include_directories(nsfplay_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(nsfplay_core PRIVATE cxx_std_17)

target_compile_options(nsfplay_core PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/W0>
	$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>
)

# The vendored comments are Shift-JIS converted to UTF-8 with a BOM. MSVC reads the BOM; the flag
# below is for toolchains that do not, so a byte-order mark in a comment cannot turn into code.
if (MSVC)
	target_compile_options(nsfplay_core PRIVATE /utf-8)
endif()
