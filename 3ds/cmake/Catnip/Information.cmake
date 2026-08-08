
set(CATNIP_RESERVED_IDENTIFIERS all build install clean)

function(__catnip_check_identifier id)
	string(REGEX REPLACE "[a-zA-Z0-9_]" "" invalidchars "${id}")
	if(NOT "${invalidchars}" STREQUAL "")
		message(FATAL_ERROR "Identifier contains an invalid character: ${id}")
	endif()

	string(FIND "${id}" "__" badseq)
	if(badseq GREATER_EQUAL 0)
		message(FATAL_ERROR "Identifier contains a reserved sequence: ${id}")
	endif()

	if("${id}" IN_LIST CATNIP_RESERVED_IDENTIFIERS)
		message(FATAL_ERROR "Reserved identifier: ${id}")
	endif()
endfunction()

function(__catnip_check_identifier2 id)
	string(REGEX REPLACE "[a-zA-Z0-9_-]" "" invalidchars "${id}")
	if(NOT "${invalidchars}" STREQUAL "")
		message(FATAL_ERROR "Identifier2 contains an invalid character: ${id}")
	endif()
endfunction()

function(__catnip_build_cache outvar)
	set(_entlist "")
	foreach(entry ${ARGN})
		if(NOT entry MATCHES "^([a-zA-Z0-9_]+)(:BOOL|:FILEPATH|:PATH|:STRING|:INTERNAL|:UNSET)?=")
			message(FATAL_ERROR "Invalid cache entry: ${entry}")
		endif()

		if(CMAKE_MATCH_2 STREQUAL ":UNSET")
			set(_ent_${CMAKE_MATCH_1} "")
		else()
			set(_ent_${CMAKE_MATCH_1} "${entry}")
			list(APPEND _entlist "${CMAKE_MATCH_1}")
		endif()
	endforeach()

	list(SORT _entlist)
	set(_cache "")
	foreach(var IN LISTS _entlist)
		set(entry "${_ent_${var}}")
		if(NOT entry STREQUAL "")
			set(_ent_${var} "")
			list(APPEND _cache "${entry}")
		endif()
	endforeach()

	set(${outvar} "${_cache}" PARENT_SCOPE)
endfunction()

macro(__catnip_scope var)
	get_property(${var} GLOBAL PROPERTY CATNIP_SCOPE)
	if(NOT ${var})
		message(FATAL_ERROR "catnip: missing catnip_package() call")
	endif()
endmacro()

function(__catnip_find_root)
	set(dir "")
	set(nextdir "${CMAKE_SOURCE_DIR}")
	while(NOT "${dir}" STREQUAL "${nextdir}")
		set(dir "${nextdir}")

		if(EXISTS "${dir}/.catnip_root")
			set(CATNIP_ROOT "${dir}" PARENT_SCOPE)
			return()
		endif()

		get_filename_component(nextdir "${dir}" DIRECTORY)
	endwhile(NOT "${dir}" STREQUAL "${nextdir}")

	set(CATNIP_ROOT "${CMAKE_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(__catnip_find_toolset var toolset severity)
	foreach(dir IN LISTS CMAKE_MODULE_PATH)
		file(GLOB files "${dir}/*.cmake" LIST_DIRECTORIES false)
		string(TOLOWER "${dir}/${toolset}.cmake" match1)
		foreach(file IN LISTS files)
			string(TOLOWER "${file}" match2)
			if(match1 STREQUAL match2)
				set(${var} "${file}" PARENT_SCOPE)
				return()
			endif()
		endforeach()
	endforeach()

	set(${var} "" PARENT_SCOPE)
	message("${severity}" "Could not find a suitable '${toolset}' toolset")
endfunction()

function(__catnip_add_subdirectory subdir)
	get_filename_component(subdir "${subdir}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")

	include(${subdir}/Catnip.cmake OPTIONAL RESULT_VARIABLE ret)
	if(ret)
		return()
	endif()

	if(EXISTS ${subdir}/CMakeLists.txt)
		file(READ ${subdir}/CMakeLists.txt listtext)
		if(listtext MATCHES "catnip_package")
			include(${subdir}/CMakeLists.txt)
		elseif("${subdir}" STREQUAL "${CATNIP_ROOT}")
			# Handle pure CMake projects
			catnip_package(main DIR "${subdir}" DEFAULT release)
			catnip_add_preset(release BUILD_TYPE Release)
			catnip_add_preset(debug BUILD_TYPE Debug)
			set_property(GLOBAL PROPERTY CATNIP_CWD_PACKAGE main)
		else()
			message(FATAL_ERROR "${subdir} is a pure CMake project, please use catnip_package() instead")
		endif()
		return()
	endif()

	message(FATAL_ERROR "Missing Catnip.cmake or CMakeLists.txt in ${subdir}")
endfunction()

function(catnip_add_subdirectory subdir)
	get_property(backupscope GLOBAL PROPERTY CATNIP_SCOPE)
	set_property(GLOBAL PROPERTY CATNIP_SCOPE "")
	__catnip_add_subdirectory("${subdir}")
	set_property(GLOBAL PROPERTY CATNIP_SCOPE ${backupscope})
endfunction()

function(catnip_package pkgname)
	__catnip_check_identifier("${pkgname}")
	cmake_parse_arguments(PARSE_ARGV 1 ARG
		""
		"DIR"
		"DEFAULT"
	)

	if(NOT ARG_DIR)
		set(ARG_DIR "${CMAKE_CURRENT_LIST_DIR}")
	else()
		get_filename_component(ARG_DIR "${ARG_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
	endif()

	if(NOT EXISTS "${ARG_DIR}/CMakeLists.txt")
		message(FATAL_ERROR "Missing CMakeLists.txt in ${pkgname}")
	endif()

	set(scope CATNIP_${pkgname})
	set_property(GLOBAL PROPERTY CATNIP_SCOPE ${scope})
	set_property(GLOBAL APPEND PROPERTY CATNIP_PACKAGES ${pkgname})
	set_property(GLOBAL PROPERTY ${scope}_SOURCE "${ARG_DIR}")
	set_property(GLOBAL PROPERTY ${scope}_DEFAULT "${ARG_DEFAULT}")

	if("${ARG_DIR}" STREQUAL "${CMAKE_SOURCE_DIR}")
		set_property(GLOBAL PROPERTY CATNIP_CWD_PACKAGE ${pkgname})
	endif()
endfunction()

function(catnip_add_preset presetname)
	__catnip_scope(scope)
	__catnip_check_identifier("${presetname}")
	cmake_parse_arguments(PARSE_ARGV 1 ARG
		"DEFAULT"
		"TOOLSET;BUILD_TYPE;SYSTEM;PROCESSOR"
		"CACHE;DEPENDS"
	)

	if(ARG_TOOLSET)
		__catnip_check_identifier2("${ARG_TOOLSET}")
		__catnip_find_toolset(ARG_TOOLSET "${ARG_TOOLSET}" WARNING)
		if(NOT ARG_TOOLSET)
			return()
		endif()
		list(APPEND ARG_CACHE "CMAKE_TOOLCHAIN_FILE=${ARG_TOOLSET}")
	elseif(NOT CATNIP_HAS_DEFAULT_TOOLSET)
		message(FATAL_ERROR "No default toolset specified: use -T <toolset> to select one (e.g. -T 3DS)")
	endif()

	if(ARG_BUILD_TYPE)
		__catnip_check_identifier2("${ARG_BUILD_TYPE}")
		list(APPEND ARG_CACHE "CMAKE_BUILD_TYPE=${ARG_BUILD_TYPE}")
	endif()

	if(ARG_SYSTEM)
		__catnip_check_identifier2("${ARG_SYSTEM}")
		list(APPEND ARG_CACHE "CMAKE_SYSTEM_NAME=${ARG_SYSTEM}")
	endif()

	if(ARG_PROCESSOR)
		__catnip_check_identifier2("${ARG_PROCESSOR}")
		list(APPEND ARG_CACHE "CMAKE_SYSTEM_PROCESSOR=${ARG_PROCESSOR}")
	endif()

	if(NOT ARG_CACHE STREQUAL "")
		__catnip_build_cache(ARG_CACHE ${CATNIP_CACHE} ${ARG_CACHE})
	else()
		set(ARG_CACHE "${CATNIP_CACHE}")
	endif()

	string(MD5 stamp "${ARG_CACHE}")

	set(cmakeargs "")
	foreach(cacheentry IN LISTS ARG_CACHE)
		list(APPEND cmakeargs "-D${cacheentry}")
	endforeach()

	set_property(GLOBAL APPEND PROPERTY ${scope}_PRESETS ${presetname})
	if(ARG_DEFAULT)
		set_property(GLOBAL APPEND PROPERTY ${scope}_DEFAULT ${presetname})
	endif()

	set(scope ${scope}__${presetname})
	set_property(GLOBAL PROPERTY ${scope}_STAMP "${stamp}")
	set_property(GLOBAL PROPERTY ${scope}_CMAKE_ARGS "${cmakeargs}")
	set_property(GLOBAL PROPERTY ${scope}_DEPENDS "${ARG_DEPENDS}")
endfunction()

if(NOT DEFINED CATNIP_ROOT)
	if(DEFINED ENV{CATNIP_ROOT})
		get_filename_component(CATNIP_ROOT "$ENV{CATNIP_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
	else()
		__catnip_find_root()
	endif()
else()
	get_filename_component(CATNIP_ROOT "${CATNIP_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
endif()

if(NOT DEFINED CATNIP_BUILD_DIR)
	if(DEFINED ENV{CATNIP_BUILD_DIR})
		get_filename_component(CATNIP_BUILD_DIR "$ENV{CATNIP_BUILD_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
	else()
		set(CATNIP_BUILD_DIR ${CATNIP_ROOT}/build)
	endif()
else()
	get_filename_component(CATNIP_BUILD_DIR "${CATNIP_BUILD_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
endif()

if("${CATNIP_BUILD_DIR}" STREQUAL "${CATNIP_ROOT}")
	message(FATAL_ERROR "Catnip build directory must be different than source root directory")
endif()

if(DEFINED CATNIP_DEFAULT_TOOLSET)
	__catnip_check_identifier2("${CATNIP_DEFAULT_TOOLSET}")
	__catnip_find_toolset(CATNIP_DEFAULT_TOOLSET "${CATNIP_DEFAULT_TOOLSET}" FATAL_ERROR)
	list(APPEND CATNIP_CACHE "CMAKE_TOOLCHAIN_FILE=${CATNIP_DEFAULT_TOOLSET}")
endif()

if(EXISTS "${CATNIP_BUILD_DIR}/.catnip_cache")
	file(READ "${CATNIP_BUILD_DIR}/.catnip_cache" CATNIP_OLD_CACHE)
endif()

if(NOT "${CATNIP_CACHE}" STREQUAL "")
	__catnip_build_cache(CATNIP_CACHE ${CATNIP_OLD_CACHE} ${CATNIP_CACHE})

	message(STATUS "Updating Catnip cache")
	file(MAKE_DIRECTORY "${CATNIP_BUILD_DIR}")
	file(WRITE "${CATNIP_BUILD_DIR}/.catnip_cache" "${CATNIP_CACHE}")
else()
	set(CATNIP_CACHE "${CATNIP_OLD_CACHE}")
endif()

if(CATNIP_CACHE MATCHES "^CMAKE_TOOLCHAIN_FILE[:=]" OR CATNIP_CACHE MATCHES ";CMAKE_TOOLCHAIN_FILE[:=]")
	set(CATNIP_HAS_DEFAULT_TOOLSET TRUE)
endif()

if(CATNIP_VERBOSE)
	message(STATUS "Catnip source root directory is ${CATNIP_ROOT}")
	message(STATUS "Catnip build directory is ${CATNIP_BUILD_DIR}")

	if(NOT CATNIP_CACHE STREQUAL "")
		message(STATUS "Catnip cache is ${CATNIP_CACHE}")
	endif()
endif()

catnip_add_subdirectory(${CATNIP_ROOT})
