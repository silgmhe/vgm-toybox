#### miscellaneous utility functions/classes ####
project(vgm-utils)

option(UTIL_LOADERS "build data (file/memory) loaders, requires zlib" ON)
option(UTIL_THREADING "build threading utilities (thread/mutex/signal)" ON)
option(UTIL_CHARSET_CONV "build charset conversion functions" ON)


set(UTIL_DEFS)
set(UTIL_FILES)
set(UTIL_HEADERS)	# export headers
set(UTIL_INCLUDES)
set(UTIL_LIBS)
set(UTIL_DEPS)	# dependencies for CMake Config file

set(UTIL_PC_CFLAGS)
set(UTIL_PC_LDFLAGS)
set(UTIL_PC_PKGS)



# File Functions
# --------------
if(UTIL_LOADERS)

# zlib (used for .vgz files and compressed data blocks inside VGM files)
# The zlib source code is fetched with CPM.cmake and built as a sub-project of this
# project, so that neither an installed zlib package nor a zlib DLL is required.
# Set LIBVGM_ZLIB_FROM_SOURCE=OFF to use a system/installed zlib instead.
option(LIBVGM_ZLIB_FROM_SOURCE "fetch zlib with CPM and build it from source" ON)
set(LIBVGM_ZLIB_TAG "v1.3.1" CACHE STRING "zlib git tag to fetch")
set(LIBVGM_ZLIB_MIRRORS "https://github.com/madler/zlib.git;https://gitee.com/mirrors/zlib.git"
	CACHE STRING "git repositories to probe for the zlib source (in order of preference)")
set(LIBVGM_ZLIB_REPOSITORY "" CACHE STRING "git repository to fetch zlib from (empty: probe LIBVGM_ZLIB_MIRRORS)")

if(LIBVGM_ZLIB_FROM_SOURCE AND NOT TARGET ZLIB::ZLIB)
	# pick the first reachable repository (GitHub is not accessible from everywhere)
	if(NOT LIBVGM_ZLIB_REPOSITORY)
		foreach(_zlib_repo IN LISTS LIBVGM_ZLIB_MIRRORS)
			message(STATUS "libvgm: probing zlib repository ${_zlib_repo} ...")
			file(DOWNLOAD "${_zlib_repo}/info/refs?service=git-upload-pack"
				"${CMAKE_CURRENT_BINARY_DIR}/zlib_repo_probe.txt" TIMEOUT 6 STATUS _zlib_probe_status)
			list(GET _zlib_probe_status 0 _zlib_probe_code)
			if(_zlib_probe_code EQUAL 0)
				set(LIBVGM_ZLIB_REPOSITORY "${_zlib_repo}" CACHE STRING
					"git repository to fetch zlib from" FORCE)
				break()
			endif()
		endforeach()
		file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/zlib_repo_probe.txt")
	endif()
	if(NOT LIBVGM_ZLIB_REPOSITORY)
		message(FATAL_ERROR "libvgm: no reachable zlib repository - set LIBVGM_ZLIB_REPOSITORY=<url> "
			"or build with LIBVGM_ZLIB_FROM_SOURCE=OFF")
	endif()

	include("${LIBVGM_SOURCE_DIR}/cmake/CPM.cmake")
	CPMAddPackage(
		NAME zlib
		GIT_REPOSITORY "${LIBVGM_ZLIB_REPOSITORY}"
		GIT_TAG "${LIBVGM_ZLIB_TAG}"
		DOWNLOAD_ONLY YES
		# zlib's CMakeLists.txt renames the checked-in zconf.h inside its source tree,
		# which would make CPM report its source cache as "dirty" on every re-configure.
		# Declaring a (no-op) patch command tells CPM that this modification is expected.
		PATCH_COMMAND "${CMAKE_COMMAND}" -E true
	)

	# zlib is used inside this project only - no need to build its example programs
	# or to install it. (Note: zlib's "zlib" target is the shared library.)
	set(SKIP_INSTALL_ALL ON CACHE BOOL "zlib: skip install rules")
	set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "zlib: build example programs")
	# zlib's CMakeLists.txt still requires CMake 2.4.4, which CMake 4.x rejects
	if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
		set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
	endif()
	add_subdirectory("${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
	if(TARGET zlib)
		set_target_properties(zlib PROPERTIES EXCLUDE_FROM_ALL TRUE)	# don't build the zlib DLL
	endif()

	# Note: $<TARGET_FILE:> is required here - referring to the (not exported) "zlibstatic"
	# target by name would make export()/install(EXPORT) fail ("not in any export set").
	set(UTIL_ZLIB_LIB $<BUILD_INTERFACE:$<TARGET_FILE:zlibstatic>> $<INSTALL_INTERFACE:ZLIB::ZLIB>)
	# zlib.h / generated zconf.h (needed by vgm-utils and e.g. player/gymplayer.cpp).
	# $<BUILD_INTERFACE:> keeps them out of the installed CMake package.
	set(UTIL_INCLUDES_PUB ${UTIL_INCLUDES_PUB}
		$<BUILD_INTERFACE:${zlib_SOURCE_DIR}> $<BUILD_INTERFACE:${zlib_BINARY_DIR}>)
else()
	find_package(ZLIB REQUIRED)
	set(UTIL_ZLIB_LIB ZLIB::ZLIB)
endif()

set(UTIL_DEPS ${UTIL_DEPS} "ZLIB")
set(UTIL_HEADERS ${UTIL_HEADERS} DataLoader.h FileLoader.h MemoryLoader.h)
set(UTIL_FILES ${UTIL_FILES} DataLoader.c FileLoader.c MemoryLoader.c)
set(UTIL_LIBS ${UTIL_LIBS} ${UTIL_ZLIB_LIB})
set(UTIL_PC_PKGS ${UTIL_PC_PKGS} "zlib")
endif(UTIL_LOADERS)



# Threads and Synchronization
# ---------------------------
if(UTIL_THREADING)
find_package(Threads REQUIRED)
set(UTIL_DEPS ${UTIL_DEPS} "Threads")

set(UTIL_HEADERS ${UTIL_HEADERS} OSMutex.h OSSignal.h OSThread.h)
if(CMAKE_USE_WIN32_THREADS_INIT)
	set(UTIL_FILES ${UTIL_FILES}
		OSMutex_Win.c
		OSSignal_Win.c
		OSThread_Win.c
	)
elseif(CMAKE_USE_PTHREADS_INIT)
	set(UTIL_FILES ${UTIL_FILES}
		OSMutex_POSIX.c
		OSSignal_POSIX.c
		OSThread_POSIX.c
	)
endif()
set(UTIL_LIBS ${UTIL_LIBS} Threads::Threads)
set(UTIL_PC_LDFLAGS ${UTIL_PC_LDFLAGS} ${CMAKE_THREAD_LIBS_INIT})
endif(UTIL_THREADING)



# String Codepage Conversion
# --------------------------
if(UTIL_CHARSET_CONV)
find_package(Iconv QUIET)
if(WIN32)
	if(${ICONV_FOUND})
		set(CAN_WINAPI OFF)
	else()
		set(CAN_WINAPI ON)
	endif()
else()
	set(CAN_WINAPI OFF)
endif()


option(UTIL_CHARCNV_ICONV "Charset Conversion: iconv" ${ICONV_FOUND})
option(UTIL_CHARCNV_WINAPI "Charset Conversion: WinAPI" ${CAN_WINAPI})


set(UTIL_HEADERS ${UTIL_HEADERS} StrUtils.h)
if(UTIL_CHARCNV_WINAPI)
	set(UTIL_FILES ${UTIL_FILES} StrUtils-CPConv_Win.c)
elseif(UTIL_CHARCNV_ICONV)
	find_package(Iconv REQUIRED)
	set(UTIL_DEPS ${UTIL_DEPS} "Iconv")
	set(UTIL_FILES ${UTIL_FILES} StrUtils-CPConv_IConv.c)
	set(UTIL_LIBS ${UTIL_LIBS} Iconv::Iconv)
	if(NOT Iconv_IS_BUILT_IN)
		set(UTIL_PC_LDFLAGS ${UTIL_PC_LDFLAGS} "-liconv")
	endif()
endif()
endif(UTIL_CHARSET_CONV)



add_library(${PROJECT_NAME} ${LIBRARY_TYPE} ${UTIL_FILES})
set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(${PROJECT_NAME} PUBLIC ${UTIL_DEFS})
target_include_directories(${PROJECT_NAME}
	PUBLIC $<BUILD_INTERFACE:${LIBVGM_SOURCE_DIR}> $<INSTALL_INTERFACE:${LIBVGM_INSTALL_INCLUDE_DIR}>
		${UTIL_INCLUDES_PUB}
	PRIVATE ${UTIL_INCLUDES}
)
target_link_libraries(${PROJECT_NAME} PUBLIC ${UTIL_LIBS})


pkgcfg_configure("${LIBVGM_SOURCE_DIR}/cmake/libvgm.pc.in" "${CMAKE_CURRENT_BINARY_DIR}/vgm-utils.pc"
		NAME "LibVGM Utils"
		DESC "LibVGM utility library"
		VERSION ${LIBVGM_VERSION}
		DEFINES ${UTIL_DEFS}
		CFLAGS ${UTIL_PC_CFLAGS}
		LDFLAGS_PRIV ${UTIL_PC_LDFLAGS}
		LDFLAGS_PUB "-l${PROJECT_NAME}"
		PKGS_PRIV ${UTIL_PC_PKGS}
	)

install(TARGETS ${PROJECT_NAME}
		EXPORT "${LIBVGM_NAME}"
		RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
		LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
		ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	)
set(LIBVGM_INSTALL_TARGETS ${LIBVGM_INSTALL_TARGETS} "${PROJECT_NAME}" PARENT_SCOPE)
install(FILES ${UTIL_HEADERS} DESTINATION "${LIBVGM_INSTALL_INCLUDE_DIR}/utils")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vgm-utils.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
set(LIBVGM_DEPS ${LIBVGM_DEPS} "${UTIL_DEPS}" PARENT_SCOPE)
