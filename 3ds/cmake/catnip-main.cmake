cmake_minimum_required(VERSION 3.13)

set(CATNIP TRUE)
set(CATNIP_VERBS config build install clean)

set(CATNIP_CLANGD_FLAGS_REMOVE
	-mword-relocations
)

include(${CMAKE_CURRENT_LIST_DIR}/dkp-initialize-path.cmake)
include(Catnip/Utility)
include(Catnip/Information)
include(Catnip/Topology)

if("${CATNIP_VERB}" STREQUAL "clean")
	include(Catnip/Verb-Clean)
elseif("${CATNIP_VERB}" MATCHES "^dump-.+")
	include(Catnip/Verb-Dump)
else()
	include(Catnip/Verb-Build)
endif()
