cmake_minimum_required(VERSION 3.13)
include_guard(GLOBAL)

if(NOT DEFINED CATNIP_PRESET)
	if(NOT DEFINED ENV{CATNIP_PRESET})
		return()
	endif()

	set(CATNIP_PRESET "$ENV{CATNIP_PRESET}" CACHE INTERNAL "")
	set(CATNIP_BUILD_DIR "$ENV{CATNIP_BUILD_DIR}" CACHE INTERNAL "")
endif()

function(catnip_import selector file)
	string(FIND "${selector}" "." dotpos)
	if("${dotpos}" LESS 0)
		set(selector "${selector}.${CATNIP_PRESET}")
	endif()

	include(${CATNIP_BUILD_DIR}/${selector}/${file}.cmake)
endfunction()
